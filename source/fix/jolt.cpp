#define LOGGER "jolt"

// Must precede any transitive <windows.h> include (via config.hpp/stdafx.hpp) in this
// translation unit - windows.h's min/max macros mangle Jolt's own min()/max() calls
// (e.g. Vec3::sRandom/Quat::sRandom) into syntax errors otherwise.
#define NOMINMAX

#include "ext/logger.hpp"
#include "fix/jolt.hpp"
#include "config.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>

#include "hta/m3d/Landscape.hpp"
#include "hta/m3d/RoadManager.hpp"
#include "hta/m3d/RoadNode.hpp"
#include "hta/m3d/RoadSet.hpp"
#include "hta/m3d/GeomObjectRoad.hpp"
#include "hta/m3d/AnimatedModel.hpp"
#include "routines.hpp"

#include <cfloat>
#include <cstdarg>
#include <cstdio>
#include <thread>

JPH_SUPPRESS_WARNINGS

// Stage -1 (bring-up sanity, see docs/jolt-integration-techanalysis.md §5 for the staged
// plan): get Jolt vendored/built/linked/initialized cleanly inside kraken.dll, in the
// correct post-DllMain init order, with zero effect on gameplay - no hook into
// ai::DynamicScene::StepScene yet (that's Stage 1+). Stage 0 (also implemented in this file)
// is static geometry export - landscape heightfield and road meshes, both read-only bodies
// in our own separate Jolt PhysicsSystem with no effect on the game's real ODE physics.
namespace kraken::fix::jolt {
    namespace Layers {
        static constexpr JPH::ObjectLayer NON_MOVING = 0;
        static constexpr JPH::ObjectLayer MOVING     = 1;
        static constexpr JPH::ObjectLayer NUM_LAYERS = 2;
    }

    namespace BroadPhaseLayers {
        static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
        static constexpr JPH::BroadPhaseLayer MOVING(1);
        static constexpr JPH::uint            NUM_LAYERS(2);
    }

    class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
    public:
        BPLayerInterfaceImpl() {
            m_objectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
            m_objectToBroadPhase[Layers::MOVING]     = BroadPhaseLayers::MOVING;
        }

        JPH::uint GetNumBroadPhaseLayers() const override {
            return BroadPhaseLayers::NUM_LAYERS;
        }

        JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
            return m_objectToBroadPhase[layer];
        }

        const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
            switch ((JPH::BroadPhaseLayer::Type) layer) {
                case (JPH::BroadPhaseLayer::Type) BroadPhaseLayers::NON_MOVING: return "NON_MOVING";
                case (JPH::BroadPhaseLayer::Type) BroadPhaseLayers::MOVING:     return "MOVING";
                default:                                                       return "INVALID";
            }
        }

    private:
        JPH::BroadPhaseLayer m_objectToBroadPhase[Layers::NUM_LAYERS];
    };

    class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
    public:
        bool ShouldCollide(JPH::ObjectLayer layer1, JPH::BroadPhaseLayer layer2) const override {
            switch (layer1) {
                case Layers::NON_MOVING: return layer2 == BroadPhaseLayers::MOVING;
                case Layers::MOVING:     return true;
                default:                 return false;
            }
        }
    };

    class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
    public:
        bool ShouldCollide(JPH::ObjectLayer object1, JPH::ObjectLayer object2) const override {
            switch (object1) {
                case Layers::NON_MOVING: return object2 == Layers::MOVING;
                case Layers::MOVING:     return true;
                default:                 return false;
            }
        }
    };

    // Leaked for the process lifetime, matching the rest of Kraken (e.g. entry.cpp's
    // G_CONFIG) - there's no DllMain/DLL_PROCESS_DETACH anywhere in this codebase, and
    // PhysicsSystem plus the filter/layer interfaces it holds references to must outlive
    // whatever "shutdown" would even mean here.
    static BPLayerInterfaceImpl*              g_broadPhaseLayerInterface = nullptr;
    static ObjectVsBroadPhaseLayerFilterImpl* g_objectVsBroadPhaseFilter = nullptr;
    static ObjectLayerPairFilterImpl*         g_objectLayerPairFilter    = nullptr;
    static JPH::TempAllocatorImpl*            g_tempAllocator            = nullptr;
    static JPH::JobSystemThreadPool*          g_jobSystem                = nullptr;
    static JPH::PhysicsSystem*                g_physicsSystem            = nullptr;
    // Tracks the one current landscape body so a later level load can replace it instead of
    // accumulating stale heightfields - PostServersLoad fires once per CWorld::Load, and the
    // game issues more than one of those per boot (e.g. an initial default map before the
    // real autoloaded save), confirmed empirically (two exports logged 3s apart on one run).
    static JPH::BodyID                        g_landscapeBodyId;
    static JPH::BodyID                        g_roadsBodyId; // same replace-on-reload pattern

    static void TraceImpl(const char* fmt, ...) {
        char buffer[1024];
        va_list list;
        va_start(list, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, list);
        va_end(list);
        LOG_INFO("%s", buffer);
    }

#ifdef JPH_ENABLE_ASSERTS
    static bool AssertFailedImpl(const char* expression, const char* message, const char* file, JPH::uint line) {
        LOG_ERROR("Assert failed: %s:%u: (%s) %s", file, line, expression, message ? message : "");
        return false; // don't request a breakpoint - hta.exe normally runs with no debugger attached
    }
#endif

    JPH::PhysicsSystem* GetPhysicsSystem() {
        return g_physicsSystem;
    }

    void StepPhysics(float inDeltaTime) {
        if (g_physicsSystem == nullptr)
            return;
        g_physicsSystem->Update(inDeltaTime, 1, g_tempAllocator, g_jobSystem);
    }

    // Stage 0 - static geometry export (docs/jolt-integration-techanalysis.md §5). Landscape
    // heightfield only for now; road meshes are a known gap, see the ChangeCall site below.
    //
    // Coordinate/scale facts below were confirmed by direct disassembly (not guessed) of
    // Landscape::GetLsSize/getHgtAtHfPoint/GetLsHeight/Load in this same binary:
    //   - m_heightMap is a flat row-major float[(m_mapSize+1)*(m_mapSize+1)], index =
    //     (m_mapSize+1)*row + col - GetLsSize() already returns m_mapSize+1 (the grid side
    //     length), and each sample is already a final world-space Y height (the raw
    //     uint16-per-file-cell -> float conversion happens once inside Landscape::Load(),
    //     m_heightMap itself holds the already-converted value).
    //   - World X/Z per grid cell = col/row * 8.0 (CellSize constant, VA 0x990944) with no
    //     extra origin offset - this lines up exactly with JPH::HeightFieldShapeSettings'
    //     own "mOffset + mScale * (x, sample[y*count+x], y)" formula and memory layout, so
    //     the m_heightMap pointer can be handed to Jolt directly with no repacking.
    static void ExportLandscapeHeightfield(hta::m3d::Landscape* landscape) {
        if (g_physicsSystem == nullptr)
            return;

        const int32_t sampleCount = landscape->GetLsSize();
        const float*  heights     = landscape->m_heightMap;
        if (sampleCount < 2 || heights == nullptr) {
            LOG_WARNING("Landscape heightfield export skipped: sampleCount=%d heights=%p", sampleCount, (const void*) heights);
            return;
        }

        constexpr float kCellSize = 8.0f;

        JPH::HeightFieldShapeSettings settings(
            heights,
            JPH::Vec3(0.0f, 0.0f, 0.0f),
            JPH::Vec3(kCellSize, 1.0f, kCellSize),
            (JPH::uint32) sampleCount);

        JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (result.HasError()) {
            LOG_ERROR("Jolt heightfield shape creation failed: %s", result.GetError().c_str());
            return;
        }

        JPH::BodyCreationSettings bodySettings(
            result.Get(), JPH::RVec3(0.0f, 0.0f, 0.0f), JPH::Quat::sIdentity(),
            JPH::EMotionType::Static, Layers::NON_MOVING);

        JPH::BodyInterface& bodyInterface = g_physicsSystem->GetBodyInterface();

        if (!g_landscapeBodyId.IsInvalid()) {
            bodyInterface.RemoveBody(g_landscapeBodyId);
            bodyInterface.DestroyBody(g_landscapeBodyId);
            g_landscapeBodyId = JPH::BodyID();
        }

        JPH::Body* body = bodyInterface.CreateBody(bodySettings);
        if (body == nullptr) {
            LOG_ERROR("Jolt heightfield body creation failed (out of bodies?)");
            return;
        }
        g_landscapeBodyId = body->GetID();
        bodyInterface.AddBody(g_landscapeBodyId, JPH::EActivation::DontActivate);

        float minH = FLT_MAX, maxH = -FLT_MAX;
        for (int32_t i = 0; i < sampleCount * sampleCount; ++i) {
            if (heights[i] < minH) minH = heights[i];
            if (heights[i] > maxH) maxH = heights[i];
        }

        LOG_INFO("Jolt: exported landscape heightfield as a static body (%dx%d samples, %.1f world units/cell, height range [%.1f, %.1f])",
            sampleCount, sampleCount, kCellSize, minH, maxH);
    }

    // m3d::Landscape::PostServersLoad (VA 0x5B6C40) is the one point confirmed (by
    // disassembling CWorld::Load) to run exactly once per level load, strictly after
    // Landscape::Load has fully populated m_heightMap and strictly before RoadManager reads
    // its own XML roads - i.e. the right moment for heightfield export, but too early for
    // roads (those get their own hook below). Wrapping the CALL SITE (ChangeCall on
    // CWorld::Load's `call m3d::Landscape::PostServersLoad` at VA 0x5CA413) rather than
    // PostServersLoad's own body means the original implementation runs completely untouched
    // via the normal hta:: method call below - see [[kraken-hook-conflicts]] for why patching
    // a call site instead of a function body is the lower-risk option whenever nothing else
    // already owns that exact site (confirmed via grep - nothing else in Kraken patches it).
    static void __fastcall PostServersLoadHook(hta::m3d::Landscape* landscape, void*) {
        landscape->PostServersLoad();
        ExportLandscapeHeightfield(landscape);
    }

    // Road export. Confirmed via disassembly of RoadManager::CalcNodeData (the function that
    // builds each road segment's ODE collision geom) that:
    //   - model selection is RoadManager::m_roadSets[node->m_roadSetHandle]->
    //     m_roadModels[node->m_type][node->m_modelNum] (an AnimatedModel*);
    //   - CRITICALLY, GeomObject::m_translation/m_rotation (the inherited placement fields)
    //     are NEVER written for road geoms - CalcNodeData instead bakes each vertex directly
    //     into world space itself (heading rotation toward a linked node + Landscape::
    //     GetLsHeight/getNormal terrain conforming) BEFORE handing the mesh to ODE, so
    //     GeomObjectRoad::m_Vertices (CVector*, GeomObject offset 0x54) is already the exact
    //     world-space triangle soup ODE collides against - no transform needed on our side;
    //     m_Indices (int32_t*, offset 0x58) is the matching int-converted index buffer.
    //   - Neither array carries its own element count on GeomObject/RoadNode, and ODE's
    //     dxTriMeshData is opaque (no query API) - but the SAME model lookup above also gives
    //     the exact counts safely, via AnimatedModel::m_meshes[0].m_numVertices/m_numFaces
    //     (the render mesh CalcNodeData itself copied from), so no unbounded read is needed.
    // Every road node is batched into one JPH::MeshShapeSettings/static body per level load,
    // same replace-on-reload pattern as the landscape heightfield above.
    static void ExportRoadsToJolt(hta::m3d::RoadManager* roadManager) {
        if (g_physicsSystem == nullptr)
            return;

        hta::m3d::RoadNode* root = roadManager->m_roadRoot;
        if (root == nullptr) {
            LOG_WARNING("Jolt: road export skipped, no road root yet");
            return;
        }

        JPH::VertexList         vertices;
        JPH::IndexedTriangleList triangles;
        int32_t nodeCount = 0, exportedCount = 0, skippedCount = 0;

        for (hta::m3d::Object* child = root->GetFirstChild(); child != nullptr; child = child->GetNextSibling()) {
            ++nodeCount;
            hta::m3d::RoadNode* node = static_cast<hta::m3d::RoadNode*>(child);

            hta::m3d::GeomObjectRoad* geom = node->m_geomObject;
            if (geom == nullptr || geom->m_Vertices == nullptr || geom->m_Indices == nullptr) {
                ++skippedCount;
                continue;
            }
            if (node->m_roadSetHandle < 0 || (size_t) node->m_roadSetHandle >= roadManager->m_roadSets.size()) {
                ++skippedCount;
                continue;
            }
            hta::m3d::RoadSet* roadSet = roadManager->m_roadSets[node->m_roadSetHandle];
            if (roadSet == nullptr || node->m_type < 0 || node->m_type >= 4) {
                ++skippedCount;
                continue;
            }
            auto& modelsOfType = roadSet->m_roadModels[node->m_type];
            if (node->m_modelNum >= modelsOfType.size()) {
                ++skippedCount;
                continue;
            }
            hta::m3d::AnimatedModel* model = modelsOfType[node->m_modelNum];
            if (model == nullptr || model->m_numMeshes == 0) {
                ++skippedCount;
                continue;
            }

            const hta::m3d::AnimatedModel::Mesh& mesh = model->m_meshes[0];
            const int32_t numVerts = mesh.m_numVertices;
            const int32_t numFaces = mesh.m_numFaces;
            if (numVerts <= 0 || numFaces <= 0) {
                ++skippedCount;
                continue;
            }

            const uint32_t baseIndex = (uint32_t) vertices.size();
            vertices.reserve(vertices.size() + numVerts);
            for (int32_t i = 0; i < numVerts; ++i) {
                const hta::CVector& v = geom->m_Vertices[i];
                vertices.emplace_back(v.x, v.y, v.z);
            }

            triangles.reserve(triangles.size() + numFaces);
            for (int32_t i = 0; i < numFaces; ++i) {
                const int32_t* tri = &geom->m_Indices[i * 3];
                triangles.emplace_back(baseIndex + (uint32_t) tri[0], baseIndex + (uint32_t) tri[1], baseIndex + (uint32_t) tri[2], 0u);
            }

            ++exportedCount;
        }

        if (triangles.empty()) {
            LOG_WARNING("Jolt: road export produced zero triangles (nodes=%d, skipped=%d) - hook may be firing before CalcNodeData runs", nodeCount, skippedCount);
            return;
        }

        JPH::MeshShapeSettings settings(vertices, triangles);
        JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (result.HasError()) {
            LOG_ERROR("Jolt road mesh shape creation failed: %s", result.GetError().c_str());
            return;
        }

        JPH::BodyCreationSettings bodySettings(
            result.Get(), JPH::RVec3(0.0f, 0.0f, 0.0f), JPH::Quat::sIdentity(),
            JPH::EMotionType::Static, Layers::NON_MOVING);

        JPH::BodyInterface& bodyInterface = g_physicsSystem->GetBodyInterface();

        if (!g_roadsBodyId.IsInvalid()) {
            bodyInterface.RemoveBody(g_roadsBodyId);
            bodyInterface.DestroyBody(g_roadsBodyId);
            g_roadsBodyId = JPH::BodyID();
        }

        JPH::Body* body = bodyInterface.CreateBody(bodySettings);
        if (body == nullptr) {
            LOG_ERROR("Jolt road mesh body creation failed (out of bodies?)");
            return;
        }
        g_roadsBodyId = body->GetID();
        bodyInterface.AddBody(g_roadsBodyId, JPH::EActivation::DontActivate);

        LOG_INFO("Jolt: exported roads as a static body (%d/%d nodes, %zu verts, %zu tris, %d skipped)",
            exportedCount, nodeCount, vertices.size(), triangles.size(), skippedCount);
    }

    // RoadManager::ReadRoadsFromXmlFile (VA 0x7B8550) is the last road-loading step inside
    // CWorld::Load per the sequence already confirmed for the landscape hook above. Same
    // wrap-the-call-site approach as PostServersLoadHook - ChangeCall on the CALL SITE at VA
    // 0x5CAC68 (confirmed via disasm: `mov ecx, edi; push edx; call
    // m3d::RoadManager::ReadRoadsFromXmlFile`), not Redirect on the function itself.
    //
    // Not 100% certain each RoadNode's GeomObjectRoad (built by CalcNodeData) is guaranteed
    // populated by the time this specific call returns rather than some later finalization
    // step - ExportRoadsToJolt defensively skips and counts any node whose geom/vertices/
    // indices aren't there yet instead of assuming, so a skewed skipped-vs-exported count in
    // the log is the signal to come back and move this hook later if needed.
    static int32_t __fastcall ReadRoadsFromXmlFileHook(hta::m3d::RoadManager* roadManager, void*, const char* path) {
        int32_t result = roadManager->ReadRoadsFromXmlFile(path);
        ExportRoadsToJolt(roadManager);
        return result;
    }

    void Apply() {
        const kraken::Config& config = kraken::Config::Instance();
        if (config.jolt.value == 0)
            return;

        LOG_INFO("Feature enabled - initializing Jolt Physics");

        // Strict init order (docs/jolt-integration-techanalysis.md §3.5/§6.2): allocator
        // before Factory, Factory before RegisterTypes. Safe to do here because
        // fix::jolt::Apply() runs from EntryPoint, which is called after DllMain returns
        // (see entry.cpp) - never do this from DLL_PROCESS_ATTACH under loader lock.
        JPH::RegisterDefaultAllocator();

        JPH::Trace = TraceImpl;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = AssertFailedImpl;)

        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();

        g_tempAllocator = new JPH::TempAllocatorImpl(16 * 1024 * 1024);

        uint32_t threads = config.jolt_threads.value;
        if (threads == 0) {
            uint32_t hw = std::thread::hardware_concurrency();
            threads = hw > 1 ? hw - 1 : 1;
        }
        g_jobSystem = new JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, (int) threads);

        g_broadPhaseLayerInterface = new BPLayerInterfaceImpl();
        g_objectVsBroadPhaseFilter = new ObjectVsBroadPhaseLayerFilterImpl();
        g_objectLayerPairFilter    = new ObjectLayerPairFilterImpl();

        // Placeholder capacities - nothing is added yet (Stage 0+ will size these for
        // real vehicle/static counts once we know what a level actually needs).
        constexpr JPH::uint cMaxBodies            = 1024;
        constexpr JPH::uint cNumBodyMutexes        = 0; // 0 = Jolt default
        constexpr JPH::uint cMaxBodyPairs          = 1024;
        constexpr JPH::uint cMaxContactConstraints = 1024;

        g_physicsSystem = new JPH::PhysicsSystem();
        g_physicsSystem->Init(
            cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
            *g_broadPhaseLayerInterface, *g_objectVsBroadPhaseFilter, *g_objectLayerPairFilter);

        LOG_INFO("Jolt Physics initialized (threads=%u, maxBodies=%u)", threads, cMaxBodies);

        routines::ChangeCall((void*) 0x005CA413, &PostServersLoadHook);
        routines::ChangeCall((void*) 0x005CAC68, &ReadRoadsFromXmlFileHook);
        LOG_INFO("Jolt: static geometry export hooks installed (landscape heightfield + roads)");
    }
}
