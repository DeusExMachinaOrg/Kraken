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
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>

#include "hta/m3d/Landscape.hpp"
#include "hta/m3d/RoadManager.hpp"
#include "hta/m3d/RoadNode.hpp"
#include "hta/m3d/RoadSet.hpp"
#include "hta/m3d/GeomObjectRoad.hpp"
#include "hta/m3d/AnimatedModel.hpp"
#include "ode/ode.hpp"
#include "routines.hpp"

#include <cfloat>
#include <cstdarg>
#include <cstdio>
#include <thread>
#include <vector>

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
    // One body per exported static obstacle (box/sphere) - unlike landscape/roads there's no
    // single batched mesh, so this is a list rather than one BodyID. Same replace-on-reload
    // pattern: cleared and rebuilt from scratch each time ExportStaticObstaclesToJolt runs.
    static std::vector<JPH::BodyID>           g_staticObstacleBodyIds;
    // Set to a small positive frame count whenever a level (re)load is detected (see
    // ReadRoadsFromXmlFileHook below); StepPhysics ticks it down and runs the static-obstacle
    // export exactly ONCE, on the frame the count reaches zero - level loading is synchronous on
    // this engine's main thread, so even that first post-load frame is already strictly after
    // CWorld::Load has fully returned. A short delay margin costs nothing; re-running the export
    // itself on every one of those frames does NOT (docs §22.9-fix) - it rebuilds every exported
    // shape from scratch each time, including ~1600 individual trimesh shapes on a typical level,
    // and doing that ~30x in a row on every level load was confirmed live to noticeably slow down
    // loading. Confirmed live across multiple runs that the geom set is already stable by the
    // first post-load frame anyway (identical counts whether checked immediately or later), so
    // there was never a real staggered-placement case to guard against.
    static int32_t                            g_staticsExportPendingFrames = 0;

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

    static void ExportStaticObstaclesToJolt(); // defined below, used by StepPhysics

    JPH::PhysicsSystem* GetPhysicsSystem() {
        return g_physicsSystem;
    }

    void StepPhysics(float inDeltaTime) {
        if (g_physicsSystem == nullptr)
            return;
        if (g_staticsExportPendingFrames > 0) {
            --g_staticsExportPendingFrames;
            if (g_staticsExportPendingFrames == 0)
                ExportStaticObstaclesToJolt();
        }
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

    // Static-obstacle export (docs §22.6/§22.7/§22.9) - rocks, buildings, and any other static
    // geometry sitting in ai::gGlobalSpace that isn't the landscape or roads. Confirmed root
    // cause of the live "vehicle ghosts through rocks/other vehicles" bug (§22.6): Jolt's world
    // only ever contained landscape + roads, so anything else was invisible to it.
    //
    // Unlike the landscape/road exports above, this doesn't hook a specific loader function -
    // disassembly of the one plausible XML-driven candidate (m3d::CWorld::LoadStaticObstacles,
    // VA 0x5C8670, "obstacles.xml" Boxes/Box nodes) showed it only builds ai::Obstacle/Obb
    // objects for AI navigation avoidance and links them via m3d::Landscape::LinkObstacleToCells
    // - no ODE call anywhere in that function, confirming it's unrelated to physical collision
    // (same dead-end category as m3d::GeomObjectStatics/m3d::StaticModelsServer/
    // ai::ObjContainer::LinkGeomsToCollisionCells before it, docs §22.7). Whatever loader
    // actually attaches physical ODE geoms for rocks/buildings was not identified - but it
    // doesn't need to be: any geom in ai::gGlobalSpace with no attached dBody is static under
    // ODE by definition (docs §22.7), so walking the space directly finds them regardless of
    // which subsystem created them.
    // dSpaceGetGeom (the "official" per-index accessor - real PDB symbol, called successfully,
    // no crash) turned out to be a dead stub: disassembling the vtable slot it dispatches
    // through resolved to dxSpace::getGeom(int), which is just `xor eax,eax; ret 4` in the base
    // class and is NOT overridden by dxHashSpace (confirmed via dxHashSpace's own class_overview
    // - it only overrides cleanGeoms/collide/collide2, not getGeom) - so it always returns
    // nullptr regardless of index, for this exact space subtype. Caught live: the first deployed
    // build logged "0 exported... 0 dynamic geoms skipped... 0 unexported class skipped" against
    // a nonzero total count, which only adds up if the per-geom loop body never ran at all.
    //
    // The actual working enumeration (confirmed via class_overview against game.pdb): dxSpace
    // keeps its members split across two intrusive singly-linked lists - m_firstEnabled (offset
    // 0x58) and m_firstDisabled (offset 0x5c) - chained through each dxGeom's own `next` field
    // (offset 0x20, confirmed distinct from the body-ownership `body_next` at 0x14). This is
    // populated by dxSpace::add(), which dxHashSpace inherits unmodified, so walking both lists
    // is a complete, space-subtype-agnostic enumeration - the guarantee dSpaceGetGeom was
    // supposed to provide but doesn't. dSpaceGetNumGeoms is unaffected (reads dxSpace::count
    // directly at offset 0x54, no virtual dispatch) and stays the source of the total-count log.
    static dxGeom* SpaceFirstEnabledGeom(dxSpace* space) {
        return *reinterpret_cast<dxGeom* const*>(reinterpret_cast<const uint8_t*>(space) + 0x58);
    }
    static dxGeom* SpaceFirstDisabledGeom(dxSpace* space) {
        return *reinterpret_cast<dxGeom* const*>(reinterpret_cast<const uint8_t*>(space) + 0x5c);
    }
    static dxGeom* GeomNextInSpace(dxGeom* geom) {
        return *reinterpret_cast<dxGeom* const*>(reinterpret_cast<const uint8_t*>(geom) + 0x20);
    }

    static void ExportStaticObstaclesToJolt() {
        if (g_physicsSystem == nullptr)
            return;

        // ai::gGlobalSpace (VA 0xA12940, confirmed via find_public_symbols against game.pdb -
        // `?gGlobalSpace@ai@@3PAUdxSpace@@A`) - the same space ai::NearCallback iterates
        // (docs §22.3/§22.7), declared as a raw fixed-address global the same way other fix/
        // modules read game globals (e.g. fix/gunlights.cpp's LightActivated).
        dxSpace* space = *reinterpret_cast<dxSpace* const*>(0x00A12940);
        if (space == nullptr) {
            LOG_WARNING("Jolt: static obstacle export skipped, ai::gGlobalSpace not yet created");
            return;
        }

        JPH::BodyInterface& bodyInterface = g_physicsSystem->GetBodyInterface();

        for (JPH::BodyID id : g_staticObstacleBodyIds) {
            bodyInterface.RemoveBody(id);
            bodyInterface.DestroyBody(id);
        }
        g_staticObstacleBodyIds.clear();

        const int32_t numGeoms = dSpaceGetNumGeoms(space);
        int32_t classHistogram[32] = {};
        int32_t boxCount = 0, sphereCount = 0, triMeshCount = 0, skippedDynamicCount = 0, skippedOtherClassCount = 0;
        int32_t walkedCount = 0;

        std::vector<JPH::BodyID> newBodies;
        newBodies.reserve((size_t) numGeoms);

        for (int32_t listIndex = 0; listIndex < 2; ++listIndex) {
            dxGeom* geom = (listIndex == 0) ? SpaceFirstEnabledGeom(space) : SpaceFirstDisabledGeom(space);
            for (; geom != nullptr; geom = GeomNextInSpace(geom)) {
                ++walkedCount;
                if (dGeomGetBody(geom) != nullptr) {
                    ++skippedDynamicCount; // anything ODE could move - vehicles, wheels, shells, ...
                    continue;
                }

                const int32_t geomClass = dGeomGetClass(geom);
                if (geomClass >= 0 && geomClass < 32)
                    ++classHistogram[geomClass];

                JPH::Ref<JPH::Shape> shape;
                // Trimesh vertices come back already in world space (dGeomTriMeshGetTriangle
                // applies the geom's position/rotation internally, docs §22.9) - same convention
                // as the road export above, so that branch skips the position/quaternion fetch
                // below and uses an identity transform instead.
                bool worldSpaceVertices = false;
                if (geomClass == dBoxClass) {
                    float lengths[3]; // full side lengths, not half-extents
                    dGeomBoxGetLengths(geom, lengths);
                    JPH::BoxShapeSettings settings(JPH::Vec3(lengths[0] * 0.5f, lengths[1] * 0.5f, lengths[2] * 0.5f));
                    JPH::ShapeSettings::ShapeResult result = settings.Create();
                    if (result.HasError()) {
                        LOG_WARNING("Jolt: static box shape creation failed: %s", result.GetError().c_str());
                        ++skippedOtherClassCount;
                        continue;
                    }
                    shape = result.Get();
                } else if (geomClass == dSphereClass) {
                    const float radius = (float) dGeomSphereGetRadius(geom);
                    JPH::SphereShapeSettings settings(radius);
                    JPH::ShapeSettings::ShapeResult result = settings.Create();
                    if (result.HasError()) {
                        LOG_WARNING("Jolt: static sphere shape creation failed: %s", result.GetError().c_str());
                        ++skippedOtherClassCount;
                        continue;
                    }
                    shape = result.Get();
                } else if (geomClass == dTriMeshClass) {
                    const int32_t triCount = dGeomTriMeshGetTriangleCount(geom);
                    if (triCount <= 0) {
                        ++skippedOtherClassCount;
                        continue;
                    }
                    JPH::VertexList         vertices;
                    JPH::IndexedTriangleList triangles;
                    vertices.reserve((size_t) triCount * 3);
                    triangles.reserve((size_t) triCount);
                    for (int32_t t = 0; t < triCount; ++t) {
                        float v0[4], v1[4], v2[4];
                        dGeomTriMeshGetTriangle(geom, t, v0, v1, v2);
                        const uint32_t base = (uint32_t) vertices.size();
                        vertices.emplace_back(v0[0], v0[1], v0[2]);
                        vertices.emplace_back(v1[0], v1[1], v1[2]);
                        vertices.emplace_back(v2[0], v2[1], v2[2]);
                        triangles.emplace_back(base, base + 1, base + 2, 0u);
                    }
                    JPH::MeshShapeSettings settings(vertices, triangles);
                    JPH::ShapeSettings::ShapeResult result = settings.Create();
                    if (result.HasError()) {
                        LOG_WARNING("Jolt: static trimesh shape creation failed: %s", result.GetError().c_str());
                        ++skippedOtherClassCount;
                        continue;
                    }
                    shape = result.Get();
                    worldSpaceVertices = true;
                } else {
                    // Capsule/plane/etc. - not exported yet (see docs §22.9 on the capsule
                    // axis-remap this would need). Counted per-class below so a live run shows
                    // exactly what's left.
                    ++skippedOtherClassCount;
                    continue;
                }

                JPH::RVec3 bodyPos  = JPH::RVec3(0.0f, 0.0f, 0.0f);
                JPH::Quat  bodyRot  = JPH::Quat::sIdentity();
                if (!worldSpaceVertices) {
                    const float* pos = dGeomGetPosition(geom);
                    float quat[4]; // ODE's native dQuaternion layout: {w, x, y, z}
                    dGeomGetQuaternion(geom, quat);
                    bodyPos = JPH::RVec3(pos[0], pos[1], pos[2]);
                    JPH::Quat rawRot(quat[1], quat[2], quat[3], quat[0]);
                    // dQfromR (what dGeomGetQuaternion computes from a body-less geom's own
                    // rotation matrix, docs §22.9) isn't guaranteed to return a unit quaternion
                    // for every geom in practice - hit live as repeated JPH_ASSERT
                    // "inQuat.IsNormalized()" spam (non-fatal, AssertFailedImpl always returns
                    // false, but a non-unit quaternion still means wrong/undefined rotation math
                    // downstream). Normalize defensively; fall back to identity for the
                    // degenerate near-zero case rather than feed Normalize() a ~0 vector.
                    bodyRot = (rawRot.LengthSq() > 1.0e-8f) ? rawRot.Normalized() : JPH::Quat::sIdentity();
                }

                JPH::BodyCreationSettings bodySettings(
                    shape, bodyPos, bodyRot, JPH::EMotionType::Static, Layers::NON_MOVING);

                JPH::Body* body = bodyInterface.CreateBody(bodySettings);
                if (body == nullptr) {
                    LOG_ERROR("Jolt: static obstacle body creation failed (out of bodies?)");
                    continue;
                }
                newBodies.push_back(body->GetID());
                bodyInterface.AddBody(body->GetID(), JPH::EActivation::DontActivate);
                if (geomClass == dBoxClass) ++boxCount;
                else if (geomClass == dSphereClass) ++sphereCount;
                else ++triMeshCount;
            }
        }

        const int32_t exportedCount = (int32_t) newBodies.size();
        g_staticObstacleBodyIds = std::move(newBodies);

        if (walkedCount != numGeoms) {
            LOG_WARNING("Jolt: static obstacle export - walked %d geoms via m_firstEnabled/"
                        "m_firstDisabled but dSpaceGetNumGeoms reports %d - list walk may be "
                        "missing something (nested subspaces? docs §22.9), counts below are only "
                        "over what was actually walked",
                        walkedCount, numGeoms);
        }
        LOG_INFO("Jolt: exported %d static obstacles (%d box, %d sphere, %d trimesh) - %d geoms "
                 "walked in gGlobalSpace (%d total per dSpaceGetNumGeoms), %d dynamic geoms "
                 "skipped, %d body-less geoms of unexported class skipped",
                 exportedCount, boxCount, sphereCount, triMeshCount, walkedCount, numGeoms,
                 skippedDynamicCount, skippedOtherClassCount);
        for (int32_t c = 0; c < 32; ++c) {
            if (classHistogram[c] > 0 && c != dBoxClass && c != dSphereClass && c != dTriMeshClass) {
                LOG_INFO("Jolt: static obstacle export - %d body-less geom(s) of unexported ODE class %d "
                         "(docs §22.9 - classes confirmed by disassembly: 0=sphere, 1=box, 2=capsule, "
                         "7=trimesh; others not confirmed against this binary, treat the raw ID as a "
                         "lead not a fact)",
                         classHistogram[c], c);
            }
        }
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
        // Arms the (single-shot, see StepPhysics) static-obstacle export - this is the last of
        // the two known per-level-load hooks to fire, so by the time this delay elapses,
        // CWorld::Load has certainly returned and ai::gGlobalSpace holds everything it's going
        // to hold for this level.
        g_staticsExportPendingFrames = 30;
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

        // cMaxBodies bumped from the original 1024 placeholder now that static-obstacle export
        // (docs §22.9) can add one body per body-less box/sphere geom in a level, on top of
        // landscape+roads+vehicles+wheels - real per-level obstacle counts aren't measured yet
        // (that's part of what the first live run of the new export will show), so this errs
        // generous rather than risk CreateBody silently dropping obstacles past the cap.
        constexpr JPH::uint cMaxBodies            = 8192;
        constexpr JPH::uint cNumBodyMutexes        = 0; // 0 = Jolt default
        constexpr JPH::uint cMaxBodyPairs          = 4096;
        constexpr JPH::uint cMaxContactConstraints = 2048;

        g_physicsSystem = new JPH::PhysicsSystem();
        g_physicsSystem->Init(
            cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
            *g_broadPhaseLayerInterface, *g_objectVsBroadPhaseFilter, *g_objectLayerPairFilter);

        LOG_INFO("Jolt Physics initialized (threads=%u, maxBodies=%u)", threads, cMaxBodies);

        routines::ChangeCall((void*) 0x005CA413, &PostServersLoadHook);
        routines::ChangeCall((void*) 0x005CAC68, &ReadRoadsFromXmlFileHook);
        LOG_INFO("Jolt: static geometry export hooks installed (landscape heightfield + roads + "
                 "static obstacles via gGlobalSpace walk)");
    }
}
