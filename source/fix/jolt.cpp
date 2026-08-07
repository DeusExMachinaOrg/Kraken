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
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>

#include "hta/m3d/Landscape.hpp"
#include "hta/m3d/RoadManager.hpp"
#include "hta/m3d/RoadNode.hpp"
#include "hta/m3d/RoadSet.hpp"
#include "hta/m3d/GeomObjectRoad.hpp"
#include "hta/m3d/AnimatedModel.hpp"
#include "ode/ode.hpp"
#include "routines.hpp"

// docs §55: needed only to enumerate live vehicles' ODE bodies (CollectLiveVehicleBodies) so the
// static-obstacle exporter can tell "a moving vehicle" apart from "a resting breakable prop" -
// this file otherwise has no vehicle-object concerns (fix/joltshadow.cpp owns those).
#include "hta/ai/CServer.hpp"
#include "hta/ai/ObjContainer.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/ai/Wheel.hpp"
// docs §57: needed to tag each exported "resting prop" Jolt body with the real
// hta::ai::BreakableObject* that owns it, so a Jolt-side contact can call SetState() directly -
// see the dGeomGetData/PhysicBody::GetOwner()/cast<BreakableObject>() resolution inline in
// WalkSpaceForStaticExport (docs §57.4).
#include "hta/ai/BreakableObject.hpp"
#include "hta/ai/PhysicBody.hpp"

#include <cfloat>
#include <cstdarg>
#include <cstdio>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
        static constexpr JPH::ObjectLayer NON_MOVING  = 0;
        static constexpr JPH::ObjectLayer MOVING      = 1;
        // docs §23.5: a query-only "pretend" layer for VehicleCollisionTesterRay's wheel-
        // ground raycast - never assigned to a real body (so it needs no BroadPhaseLayer
        // mapping in BPLayerInterfaceImpl below; Jolt's DefaultBroadPhaseLayerFilter/
        // DefaultObjectLayerFilter only ever call ShouldCollide(WHEEL_QUERY, candidate) on
        // the filters below, never GetBroadPhaseLayer(WHEEL_QUERY) - confirmed by reading
        // PhysicsSystem::GetDefaultBroadPhaseLayerFilter/GetDefaultLayerFilter). Restricted
        // to NON_MOVING only, unlike MOVING (which collides with everything) - without this,
        // a wheel raycast in a dense vehicle crowd hits neighboring vehicles' kinematic
        // mirror bodies (docs §22.11) instead of the ground, since those share the MOVING
        // layer with the ray's own vehicle. Live-observed symptom: suspension stuck fully
        // extended and zero tire traction (vehicle slides instead of driving) whenever
        // surrounded by other vehicles.
        static constexpr JPH::ObjectLayer WHEEL_QUERY = 2;
        // docs §34: a REAL layer (unlike WHEEL_QUERY) for the auxiliary wheel-proxy bodies -
        // small dynamic spheres, slider-constrained to each vehicle's chassis, that give Jolt's
        // raycast-only wheels some physical presence for the one case they can't otherwise
        // handle (a chassis tipped far enough that the wheel's own raycast no longer reaches
        // the ground - see joltshadow.cpp's BuildWheelProxy). Restricted to NON_MOVING only,
        // same reasoning as WHEEL_QUERY: these must brace against terrain, never against other
        // vehicles' bodies or kinematic mirrors (which share MOVING) - a proxy sphere shoving a
        // neighboring vehicle around would be its own new bug, not a fix.
        static constexpr JPH::ObjectLayer WHEEL_PROXY = 3;
        // docs §58 (Этап 1, шаг 2): the real wheel BODIES. Unlike WHEEL_PROXY, which only ever
        // braces against terrain, a real wheel has to collide with everything a wheel can hit -
        // terrain AND other vehicles - so it reaches both broad-phase layers, like MOVING.
        //
        // It is a separate layer from MOVING rather than a reuse of it purely so the filter stays
        // cheap to change: wheel-vs-wheel and wheel-vs-chassis of the SAME vehicle are excluded by
        // the collision GROUP (joltshadow.cpp's GetWheelGroupFilter), not by the layer, so the
        // layer only has to answer the coarse question. Plan §6 "Риск островов" notes the cost of
        // letting WHEEL see MOVING: two vehicles can merge into one simulation island through
        // their wheels. That is accepted deliberately - wheels that cannot hit another vehicle
        // would be worse than a large island.
        static constexpr JPH::ObjectLayer WHEEL       = 4;
        static constexpr JPH::ObjectLayer NUM_LAYERS  = 5; // BPLayerInterfaceImpl's array size - WHEEL_QUERY (2) is query-only and excluded, but WHEEL_PROXY (3) and WHEEL (4) are real body layers and need slots
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
            m_objectToBroadPhase[Layers::WHEEL_PROXY] = BroadPhaseLayers::MOVING; // it's a real dynamic body, just a restricted-collision one
            m_objectToBroadPhase[Layers::WHEEL]       = BroadPhaseLayers::MOVING; // docs §58: a real dynamic wheel body
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
                case Layers::NON_MOVING:  return layer2 == BroadPhaseLayers::MOVING;
                case Layers::MOVING:      return true;
                case Layers::WHEEL_QUERY: return layer2 == BroadPhaseLayers::NON_MOVING;
                case Layers::WHEEL_PROXY: return layer2 == BroadPhaseLayers::NON_MOVING;
                case Layers::WHEEL:       return true; // docs §58: terrain AND other vehicles
                default:                  return false;
            }
        }
    };

    class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
    public:
        bool ShouldCollide(JPH::ObjectLayer object1, JPH::ObjectLayer object2) const override {
            switch (object1) {
                // docs §34: explicitly lists WHEEL_PROXY here too (not just relying on
                // WHEEL_PROXY's own case below) since it's unconfirmed whether Jolt always
                // queries this filter with the same (object1, object2) order - safer to make
                // the NON_MOVING/WHEEL_PROXY relationship symmetric in both directions.
                case Layers::NON_MOVING:  return object2 == Layers::MOVING || object2 == Layers::WHEEL_PROXY
                                              || object2 == Layers::WHEEL; // docs §58: same symmetry hedge as WHEEL_PROXY above
                case Layers::MOVING:      return true;
                case Layers::WHEEL_QUERY: return object2 == Layers::NON_MOVING;
                case Layers::WHEEL_PROXY: return object2 == Layers::NON_MOVING;
                // docs §58: a real wheel collides with terrain, with other vehicles, and with
                // other vehicles' wheels. Its OWN vehicle's chassis and sibling wheels are
                // excluded by the collision group, not here - see GetWheelGroupFilter.
                case Layers::WHEEL:       return object2 == Layers::NON_MOVING || object2 == Layers::MOVING
                                              || object2 == Layers::WHEEL;
                default:                  return false;
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

    uint32_t GetRoadsBodyRawId() {
        return g_roadsBodyId.GetIndexAndSequenceNumber();
    }

    void StepPhysics(float inDeltaTime) {
        if (g_physicsSystem == nullptr)
            return;
        if (g_staticsExportPendingFrames > 0) {
            --g_staticsExportPendingFrames;
            if (g_staticsExportPendingFrames == 0)
                ExportStaticObstaclesToJolt();
        }
        // docs §54 (Этап 1, шаг -1E): the return value used to be discarded. Jolt does not throw,
        // assert (in Release) or log when it runs out of body pairs / contact constraints / step
        // listeners - it returns a bitmask here and silently drops the excess work, so the only
        // symptom is physics quietly going wrong under load (bodies interpenetrating or falling
        // through the world in a crowd). Logged once per distinct error mask rather than per
        // frame: these conditions persist for as long as the scene is dense, and a per-frame log
        // would itself become a performance problem (the same aggregate-then-report discipline
        // used for the static-export capacity failures, docs §35).
        const JPH::EPhysicsUpdateError err =
            g_physicsSystem->Update(inDeltaTime, 1, g_tempAllocator, g_jobSystem);
        if (err != JPH::EPhysicsUpdateError::None) {
            static JPH::EPhysicsUpdateError s_lastReportedError = JPH::EPhysicsUpdateError::None;
            if (err != s_lastReportedError) {
                s_lastReportedError = err;
                LOG_ERROR("Jolt: PhysicsSystem::Update reported error mask 0x%X%s%s%s%s - simulation work was DROPPED this step (raise the corresponding limit in Apply())",
                    (unsigned) err,
                    ((JPH::uint) err & (JPH::uint) JPH::EPhysicsUpdateError::ManifoldCacheFull) ? " ManifoldCacheFull" : "",
                    ((JPH::uint) err & (JPH::uint) JPH::EPhysicsUpdateError::BodyPairCacheFull)  ? " BodyPairCacheFull"  : "",
                    ((JPH::uint) err & (JPH::uint) JPH::EPhysicsUpdateError::ContactConstraintsFull) ? " ContactConstraintsFull" : "",
                    "");
            }
        }
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

    // docs §55: bodies belonging to live vehicles (chassis + wheels) must never be treated as a
    // resting static prop by WalkSpaceForStaticExport below - a moving vehicle would otherwise
    // freeze a stale collision snapshot of itself in Jolt's world while the real (mirrored or
    // Jolt-driven) vehicle drives away. Same enumeration primitive fix/joltshadow.cpp's
    // MirrorOtherVehicles already uses (hta::ai::CServer::Instance()->m_pObjects,
    // updatingBegin/updatingEnd, Obj::cast<Vehicle>()) - duplicated here rather than shared across
    // translation units, since this file has never previously needed vehicle-object visibility
    // and joltshadow.cpp owns that concern; a minimal, self-contained local walk was judged
    // lower-risk than threading a cross-TU dependency through for one query.
    //
    // Chassis body identity comes from dJointGetBody(wheel->m_jointID, 0) (body1 side of any of
    // the vehicle's Hinge2 wheel joints - all wheels on one vehicle share the same chassis body),
    // NOT vehicle->m_body->_id - joltshadow.cpp's docs §30 found the latter reads a stale/zero
    // position for a ComplexPhysicObj vehicle's chassis (a different, non-collision bookkeeping
    // body), so its pointer identity can't be trusted either. Each wheel's own body
    // (wheel->m_body->_id) IS already proven correct elsewhere (joltshadow.cpp's §22.3 ramming
    // diagnostic calls dBodyGetNumJoints(wheel->m_body->_id) successfully), so that one's used
    // directly.
    static void CollectLiveVehicleBodies(std::unordered_set<const void*>& outBodies) {
        hta::ai::CServer* server = hta::ai::CServer::Instance();
        if (server == nullptr || server->m_pObjects == nullptr)
            return;

        hta::ai::ObjContainer* objects = server->m_pObjects;
        for (hta::ai::ObjContainer::iterator it = objects->updatingBegin(); it != objects->updatingEnd(); ++it) {
            hta::ai::Vehicle* vehicle = (*it)->cast<hta::ai::Vehicle>();
            if (vehicle == nullptr)
                continue;

            dxBody* chassisBody = nullptr;
            const uint32_t numWheels = vehicle->GetNumWheels();
            for (uint32_t i = 0; i < numWheels; ++i) {
                const hta::ai::Vehicle::WheelRuntimeInfo& info = vehicle->m_wheels[i];
                if (!info.m_bWheelPresent || info.m_wheel == nullptr)
                    continue;
                if (info.m_wheel->m_body != nullptr && info.m_wheel->m_body->_id != nullptr)
                    outBodies.insert(info.m_wheel->m_body->_id);
                if (chassisBody == nullptr && info.m_wheel->m_jointID != nullptr)
                    chassisBody = dJointGetBody(info.m_wheel->m_jointID, 0);
            }
            if (chassisBody != nullptr)
                outBodies.insert(chassisBody);
        }
    }

    // Accumulator threaded through the recursive space walk (docs §22.13) - one instance per
    // ExportStaticObstaclesToJolt call, shared by every recursive WalkSpaceForStaticExport frame,
    // so a single summary can be logged once the whole tree (top-level space + any nested
    // sub-spaces) has been walked.
    struct StaticExportContext {
        JPH::BodyInterface&       bodyInterface;
        std::vector<JPH::BodyID>& newBodies;
        // docs §55: bodies excluded from the "resting prop" path below because they're a live
        // vehicle/wheel (see CollectLiveVehicleBodies) - anything else with a body is assumed to
        // be a resting breakable prop (tree/fence/barrel/etc, docs §55) and gets a static Jolt
        // snapshot like any body-less geom.
        const std::unordered_set<const void*>& vehicleBodies;
        int32_t classHistogram[32] = {};
        int32_t boxCount = 0, sphereCount = 0, triMeshCount = 0, capsuleCount = 0;
        // Subset of the counts above that came from a resting non-vehicle dynamic body rather
        // than a genuinely body-less static geom (docs §55) - tracked separately purely for log
        // visibility, they're stored and cleaned up identically either way.
        int32_t propCount = 0;
        // docs §57: subset of propCount that got a real BreakableObject* tag (see
        // ResolveBreakableOwner above) - tracked separately so a live run shows how much of the
        // prop bucket is actually reachable for the break-on-ram path, versus untagged (owner not
        // found - e.g. not a BreakableObject, or not IsDestroyable()).
        int32_t taggedBreakableCount = 0;
        // docs §59: funnel breakdown of WHY the rest of propCount didn't tag (ResolveBreakableOwner
        // above) - §57.3 left "6284/8710 untagged, not diagnosed further" as a known gap; a live
        // "rammed a tree, nothing happened" report made it the highest-impact thing left to fix,
        // since a player has no way to tell a tagged prop from an untagged one before hitting it.
        // (docs §61: the gap turned out to be entirely unresolvedNotDestroyable, i.e. trees - now
        // tagged too, see ResolveBreakableOwner's own comment, so that counter is gone; the other
        // three remain genuinely unresolvable - no owner, not a PhysicBody, or not a BreakableObject
        // at all.)
        int32_t unresolvedNoData = 0, unresolvedNotPhysicBody = 0, unresolvedNoOwner = 0,
                unresolvedNotBreakable = 0;
        int32_t skippedDynamicCount = 0, skippedOtherClassCount = 0;
        // Geoms that had an exportable class but hit CreateBody()==nullptr (cMaxBodies
        // exhausted) - aggregated instead of logged per-geom (docs §35), see the
        // comment at the CreateBody call site for why.
        int32_t skippedCapacityCount = 0;
        int32_t walkedCount = 0;
        int32_t nestedSpaceCount = 0;
    };

    // docs §57.4 (goal: "деревья падали при таране как с ODE"): resolves the real
    // hta::ai::BreakableObject* that owns a given ODE geom, if any, so
    // WalkSpaceForStaticExport can tag the Jolt body it builds for that geom (docs §55's "resting
    // prop" path) with a pointer a Jolt-side contact listener can later call SetState() on
    // (fix/joltshadow.cpp's HandleBreakableContact) - §56's raw disassembly digging found the
    // NATIVE ODE dispatch that would normally do this (ai::NearCallback ->
    // ai::ColliderKrnl::CollideObjs -> ai::CollideVehicleAndBreakableObject) never actually fires
    // for the Jolt-driven vehicle (confirmed live via a hook, zero firings across two harness ram
    // tests that both ended in the vehicle wedged against a tree) despite every gate checked
    // along that path passing (dGeomIsEnabled always true) - root cause not pinned down further
    // (would need a hook on NearCallback itself, a meaningfully bigger risk), so this synthesizes
    // the same real-game effect entirely on Jolt's own side instead, the same strategy docs
    // §23.11 already proved out for ram PUSHBACK specifically (native dispatch unreliable there
    // too under apply=1) via VehiclePushbackContactListener.
    //
    // Goes backward from the geom (dGeomGetData -> ai::PhysicBody::GetOwner() ->
    // cast<BreakableObject>()) - the SAME chain ai::NearCallback itself uses on every body-having
    // geom it processes (docs §56.2's disassembly: dGeomGetData immediately followed by an
    // IsKindOf check then PhysicBody::GetOwner()) - reusing a mechanism the game's own working
    // collision dispatch already relies on, rather than inventing a second, independent one. A
    // FORWARD attempt (enumerate BreakableObjects via hta::ai::CServer's m_pObjects, walk their
    // own body's geom list) was tried first and found 0/~8700 matches live - m_pObjects turned
    // out to only hold a few hundred "actively simulated" entities (vehicles, AI, etc), not the
    // thousands of static, usually-dormant-until-hit world props - confirmed by adding counters
    // (docs §57.3) and seeing "0 matched cast<BreakableObject>" against a scanned total nowhere
    // near the known prop count from data/gamedata's own XML.
    static hta::ai::BreakableObject* ResolveBreakableOwner(dxGeom* geom, StaticExportContext& ctx) {
        void* geomData = dGeomGetData(geom);
        if (geomData == nullptr) {
            ++ctx.unresolvedNoData;
            return nullptr;
        }
        // docs §57.4: the safety check NearCallback itself performs before trusting this pointer
        // as a PhysicBody* (docs §56.2's disassembly - IsKindOf comes BEFORE GetOwner() there,
        // never skipped) - dGeomGetData's contract for an arbitrary geom is otherwise unverified,
        // and this runs across ~8700+ geoms at level-load, not a handful, so a bad cast here
        // isn't a one-off risk. 0xA00CB0 = ai::PhysicBody::m_classPhysicBody's own address
        // (0x400000 image base + RVA 0x600cb0, confirmed via tools/lora's symbol_at_rva against
        // game.pdb - independently identifies the exact constant NearCallback's own disassembly
        // pushes before its IsKindOf call, not guessed from context).
        hta::m3d::Object* asObject = reinterpret_cast<hta::m3d::Object*>(geomData);
        if (!asObject->IsKindOf(reinterpret_cast<const hta::m3d::Class*>(0xA00CB0))) {
            ++ctx.unresolvedNotPhysicBody;
            return nullptr;
        }
        hta::ai::PhysicBody* physicBody = reinterpret_cast<hta::ai::PhysicBody*>(geomData);
        hta::ai::PhysicObj* owner = physicBody->GetOwner();
        if (owner == nullptr) {
            ++ctx.unresolvedNoOwner;
            return nullptr;
        }
        hta::ai::BreakableObject* breakable = owner->cast<hta::ai::BreakableObject>();
        if (breakable == nullptr) {
            ++ctx.unresolvedNotBreakable;
            return nullptr;
        }
        // docs §61 (goal: "в jolt деревья падали как в ode"): used to reject here when
        // !IsDestroyable() (counted as unresolvedNotDestroyable) - that made every tree
        // permanently untaggable, since IsDestroyable()==false for every tree prototype (docs
        // §59.2's XML table). §60.3 found trees don't break via this destroyable path at all - they
        // go through SetState(ENABLED)/SetJointAnchor instead (joltshadow.cpp's
        // DrainPendingEnables), which needs the SAME tag. Both destroyable and non-destroyable
        // objects are now tagged; DrainPendingBreaks/DrainPendingEnables each re-check
        // IsDestroyable() themselves to route to the correct one.
        return breakable;
    }

    // Walks one dxSpace's geoms (both m_firstEnabled/m_firstDisabled lists), recursing into any
    // nested sub-space it finds (docs §22.13) - a body-less geom whose class falls in
    // dFirstSpaceClass..dLastSpaceClass is itself a dxSpace (dxSpace inherits dxGeom), with its
    // own m_firstEnabled/m_firstDisabled lists - the original single-level walk (docs §22.9)
    // missed whatever these contained (154 dxSimpleSpace + at least 1 other space-class instance
    // on the test level, per that pass's own class histogram). depth is a defensive cap against
    // unexpectedly deep or (in the face of a data-model surprise) circular nesting - real ODE
    // usage should never approach it.
    static void WalkSpaceForStaticExport(dxSpace* space, StaticExportContext& ctx, int depth) {
        constexpr int kMaxRecursionDepth = 8;
        if (depth > kMaxRecursionDepth) {
            LOG_WARNING("Jolt: static obstacle export - hit max sub-space recursion depth (%d), stopping this branch", kMaxRecursionDepth);
            return;
        }

        for (int32_t listIndex = 0; listIndex < 2; ++listIndex) {
            dxGeom* geom = (listIndex == 0) ? SpaceFirstEnabledGeom(space) : SpaceFirstDisabledGeom(space);
            for (; geom != nullptr; geom = GeomNextInSpace(geom)) {
                ++ctx.walkedCount;
                dxBody* odeBody = dGeomGetBody(geom);
                // docs §55: only a LIVE VEHICLE body is skipped now - it's already correctly
                // handled live (Jolt-driven writeback or fix/joltshadow.cpp's kinematic mirror/AI
                // shadow), and giving it a second, stale static snapshot here would freeze a
                // ghost collider where it happened to be at export time. Anything else with a
                // body (shells, and above all the thousands of Class="BreakableObject" world
                // props - trees, fences, barrels, posts, walls, all confirmed via
                // data/gamedata/gameobjects/breakableobjects.xml to carry a Mass, i.e. a real ODE
                // body, presumably so the game can react physically when one is rammed) falls
                // through to the SAME export path body-less static geoms already use below - see
                // isProp/ctx.propCount.
                if (odeBody != nullptr && ctx.vehicleBodies.count(odeBody) != 0) {
                    ++ctx.skippedDynamicCount;
                    continue;
                }
                const bool isProp = odeBody != nullptr;

                const int32_t geomClass = dGeomGetClass(geom);
                if (geomClass >= 0 && geomClass < 32)
                    ++ctx.classHistogram[geomClass];

                if (geomClass >= dFirstSpaceClass && geomClass <= dLastSpaceClass) {
                    ++ctx.nestedSpaceCount;
                    WalkSpaceForStaticExport(reinterpret_cast<dxSpace*>(geom), ctx, depth + 1);
                    continue;
                }

                // docs §55: unwrap ai::GeomTransform (ODE geom class 6, confirmed via
                // BuildChassisCompoundShape's own already-working unwrap, fix/joltshadow.cpp docs
                // §23.10) - a generic "shape at an offset from its owner" wrapper used throughout
                // this engine, previously the single biggest bucket of body-less geoms this
                // export silently dropped (class-6 histogram entries logged below). The INNER
                // geom's position/rotation is always LOCAL to the transform by ODE's own
                // dGeomTransform contract, regardless of whether the outer transform has a body -
                // compose outer-world (read the normal way, works whether or not geom has a body)
                // with inner-local exactly like BuildChassisCompoundShape does.
                dxGeom*    shapeGeom       = geom;
                bool       isTransformWrapped = false;
                JPH::RVec3 outerWorldPos   = JPH::RVec3(0.0f, 0.0f, 0.0f);
                JPH::Quat  outerWorldRot   = JPH::Quat::sIdentity();
                if (geomClass == dGeomTransformClass) {
                    dxGeom* inner = dGeomTransformGetGeom(geom);
                    if (inner == nullptr) {
                        ++ctx.skippedOtherClassCount;
                        continue;
                    }
                    shapeGeom = inner;
                    isTransformWrapped = true;
                    const float* outerPos = dGeomGetPosition(geom);
                    float outerQuat[4];
                    dGeomGetQuaternion(geom, outerQuat);
                    outerWorldPos = JPH::RVec3(outerPos[0], outerPos[1], outerPos[2]);
                    JPH::Quat rawOuterRot(outerQuat[1], outerQuat[2], outerQuat[3], outerQuat[0]);
                    outerWorldRot = (rawOuterRot.LengthSq() > 1.0e-8f) ? rawOuterRot.Normalized() : JPH::Quat::sIdentity();
                }
                const int32_t shapeClass = isTransformWrapped ? dGeomGetClass(shapeGeom) : geomClass;

                JPH::Ref<JPH::Shape> shape;
                // Trimesh vertices come back already in world space (dGeomTriMeshGetTriangle
                // applies the geom's position/rotation internally, docs §22.9) - same convention
                // as the road export above, so that branch skips the position/quaternion fetch
                // below and uses an identity transform instead.
                bool worldSpaceVertices = false;
                // Extra LOCAL-frame rotation the shape needs before the geom's own world
                // rotation is applied (docs §22.13) - only capsule needs this (identity/no-op
                // for everything else).
                JPH::Quat localShapeRemap = JPH::Quat::sIdentity();
                if (shapeClass == dBoxClass) {
                    float lengths[3]; // full side lengths, not half-extents
                    dGeomBoxGetLengths(shapeGeom, lengths);
                    JPH::BoxShapeSettings settings(JPH::Vec3(lengths[0] * 0.5f, lengths[1] * 0.5f, lengths[2] * 0.5f));
                    JPH::ShapeSettings::ShapeResult result = settings.Create();
                    if (result.HasError()) {
                        LOG_WARNING("Jolt: static box shape creation failed: %s", result.GetError().c_str());
                        ++ctx.skippedOtherClassCount;
                        continue;
                    }
                    shape = result.Get();
                } else if (shapeClass == dSphereClass) {
                    const float radius = (float) dGeomSphereGetRadius(shapeGeom);
                    JPH::SphereShapeSettings settings(radius);
                    JPH::ShapeSettings::ShapeResult result = settings.Create();
                    if (result.HasError()) {
                        LOG_WARNING("Jolt: static sphere shape creation failed: %s", result.GetError().c_str());
                        ++ctx.skippedOtherClassCount;
                        continue;
                    }
                    shape = result.Get();
                } else if (shapeClass == dTriMeshClass) {
                    // docs §55: same conservative scope limit BuildChassisCompoundShape already
                    // draws for vehicle parts - a transform-wrapped trimesh's vertex space
                    // (already-world, like every other trimesh here, or local-to-transform like
                    // box/sphere/capsule?) is unverified, so skip rather than risk silently wrong
                    // geometry. Not known to occur in practice; counted like any other skip.
                    if (isTransformWrapped) {
                        ++ctx.skippedOtherClassCount;
                        continue;
                    }
                    const int32_t triCount = dGeomTriMeshGetTriangleCount(shapeGeom);
                    if (triCount <= 0) {
                        ++ctx.skippedOtherClassCount;
                        continue;
                    }
                    JPH::VertexList         vertices;
                    JPH::IndexedTriangleList triangles;
                    vertices.reserve((size_t) triCount * 3);
                    triangles.reserve((size_t) triCount);
                    for (int32_t t = 0; t < triCount; ++t) {
                        float v0[4], v1[4], v2[4];
                        dGeomTriMeshGetTriangle(shapeGeom, t, v0, v1, v2);
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
                        ++ctx.skippedOtherClassCount;
                        continue;
                    }
                    shape = result.Get();
                    worldSpaceVertices = true;
                } else if (shapeClass == dCCylinderClass) {
                    float radius = 0.0f, length = 0.0f;
                    dGeomCCylinderGetParams(shapeGeom, &radius, &length);
                    if (radius <= 0.0f || length <= 0.0f) {
                        ++ctx.skippedOtherClassCount;
                        continue;
                    }
                    JPH::CapsuleShapeSettings settings(length * 0.5f, radius);
                    JPH::ShapeSettings::ShapeResult result = settings.Create();
                    if (result.HasError()) {
                        LOG_WARNING("Jolt: static capsule shape creation failed: %s", result.GetError().c_str());
                        ++ctx.skippedOtherClassCount;
                        continue;
                    }
                    shape = result.Get();
                    // ODE's CCylinder/capsule long axis is local Z; Jolt's CapsuleShape long
                    // axis is local Y (docs §22.9/§22.13, not previously implemented for exactly
                    // this reason). Rotating +90deg around local X maps Y (0,1,0) -> Z (0,0,1)
                    // (cos90=0, sin90=1), so applying this BEFORE the geom's own world rotation
                    // puts the capsule's long axis where ODE's local Z actually points once
                    // world-rotated.
                    localShapeRemap = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), JPH::DegreesToRadians(90.0f));
                } else {
                    // Plane/etc. - not exported yet. Counted per-class below so a live run shows
                    // exactly what's left.
                    ++ctx.skippedOtherClassCount;
                    continue;
                }

                JPH::RVec3 bodyPos  = JPH::RVec3(0.0f, 0.0f, 0.0f);
                JPH::Quat  bodyRot  = JPH::Quat::sIdentity();
                if (!worldSpaceVertices) {
                    const float* pos = dGeomGetPosition(shapeGeom);
                    float quat[4]; // ODE's native dQuaternion layout: {w, x, y, z}
                    dGeomGetQuaternion(shapeGeom, quat);
                    JPH::RVec3 rawPos(pos[0], pos[1], pos[2]);
                    JPH::Quat  rawRot(quat[1], quat[2], quat[3], quat[0]);
                    // dQfromR (what dGeomGetQuaternion computes from a body-less geom's own
                    // rotation matrix, docs §22.9) isn't guaranteed to return a unit quaternion
                    // for every geom in practice - hit live as repeated JPH_ASSERT
                    // "inQuat.IsNormalized()" spam (non-fatal, AssertFailedImpl always returns
                    // false, but a non-unit quaternion still means wrong/undefined rotation math
                    // downstream). Normalize defensively; fall back to identity for the
                    // degenerate near-zero case rather than feed Normalize() a ~0 vector.
                    rawRot = (rawRot.LengthSq() > 1.0e-8f) ? rawRot.Normalized() : JPH::Quat::sIdentity();
                    if (isTransformWrapped) {
                        bodyPos = outerWorldPos + outerWorldRot * rawPos;
                        bodyRot = outerWorldRot * rawRot;
                    } else {
                        bodyPos = rawPos;
                        bodyRot = rawRot;
                    }
                    bodyRot = bodyRot * localShapeRemap; // no-op (identity) for everything but capsule
                }

                // docs §55: still EMotionType::Static even for a resting prop (isProp) - a
                // one-time snapshot at export time, not a live mirror. Known, accepted gap: if a
                // prop gets broken/moved after this export, its Jolt collider stays behind until
                // the next export (level/save load) - same tradeoff already accepted for "static"
                // meaning "as of export time" everywhere else in this function. A live per-frame
                // mirror (fix/joltshadow.cpp's MirrorOtherVehicles pattern) was considered and
                // rejected: thousands of these exist per level (docs §55 live count: ~4200+ on one
                // save) versus the handful of vehicles that pattern was sized for.
                JPH::BodyCreationSettings bodySettings(
                    shape, bodyPos, bodyRot, JPH::EMotionType::Static, Layers::NON_MOVING);

                JPH::Body* body = ctx.bodyInterface.CreateBody(bodySettings);
                if (body == nullptr) {
                    // Aggregated, not logged per-geom (docs §35) - a level whose exportable
                    // static count exceeds cMaxBodies can hit this tens of thousands of times in
                    // one export call (live-confirmed: 21566 times on one real save), and logging
                    // each one individually was itself a real cost (the resulting 21566-line spam
                    // dwarfed the rest of that session's entire log). Same aggregate-then-summarize
                    // pattern already used for skippedOtherClassCount below.
                    ++ctx.skippedCapacityCount;
                    continue;
                }
                // docs §57: tag with the owning BreakableObject* (resolved via ResolveBreakableOwner
                // from THIS geom - the OUTER one, pre-unwrap, matching ResolveBreakableOwner's own
                // dGeomGetData(geom) call; a transform-wrapped shape is looked up by its outer
                // geom, never shapeGeom), so HandleBreakableContact (joltshadow.cpp) can act on a
                // real ram without going through the native dispatch docs §56 found never fires
                // here. Only ever set for isProp bodies - vehicles/wheels are tagged separately
                // (their own Vehicle*/'WHL' scheme) and plain static geoms (roads, rocks,
                // landscape) get no tag at all (UserData left at Jolt's default 0), exactly what
                // lets HandleBreakableContact tell "a tagged prop" apart from "any other static
                // body" by a single != 0 check.
                if (isProp) {
                    hta::ai::BreakableObject* owner = ResolveBreakableOwner(geom, ctx);
                    if (owner != nullptr) {
                        body->SetUserData(reinterpret_cast<uint64_t>(owner));
                        ++ctx.taggedBreakableCount;
                    }
                }
                ctx.newBodies.push_back(body->GetID());
                ctx.bodyInterface.AddBody(body->GetID(), JPH::EActivation::DontActivate);
                if (shapeClass == dBoxClass) ++ctx.boxCount;
                else if (shapeClass == dSphereClass) ++ctx.sphereCount;
                else if (shapeClass == dCCylinderClass) ++ctx.capsuleCount;
                else ++ctx.triMeshCount;
                if (isProp) ++ctx.propCount;
            }
        }
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

        // Top-level count only (dSpaceGetNumGeoms reads gGlobalSpace's own `count` field, not a
        // recursive total) - used only to size the reservation and as a lower-bound sanity check
        // below, not as "the" total any more now that nested sub-spaces are walked too.
        const int32_t topLevelCount = dSpaceGetNumGeoms(space);
        std::vector<JPH::BodyID> newBodies;
        newBodies.reserve((size_t) topLevelCount);

        // docs §55: built fresh right before each export (level load only, not per-frame) - see
        // CollectLiveVehicleBodies' own comment for why this can't just be "any body = skip"
        // any more.
        std::unordered_set<const void*> vehicleBodies;
        CollectLiveVehicleBodies(vehicleBodies);

        StaticExportContext ctx{bodyInterface, newBodies, vehicleBodies};
        WalkSpaceForStaticExport(space, ctx, 0);

        const int32_t exportedCount = (int32_t) newBodies.size();
        g_staticObstacleBodyIds = std::move(newBodies);

        if (ctx.walkedCount < topLevelCount) {
            LOG_WARNING("Jolt: static obstacle export - walked only %d geoms but dSpaceGetNumGeoms "
                        "reports %d at the top level alone - list walk may be missing something "
                        "(docs §22.9/§22.13), counts below are only over what was actually walked",
                        ctx.walkedCount, topLevelCount);
        }
        if (ctx.skippedCapacityCount > 0) {
            // Same class of bug as docs §22.14 (cMaxBodies=8192 too small for a ~32861-obstacle
            // real level, raised to 65536) recurring on an even bigger level (docs §35,
            // 87082 exportable obstacles on the "Molokovoz" mountain-terrain save alone) -
            // cMaxBodies needs raising again if this fires live.
            LOG_WARNING("Jolt: static obstacle export - %d geom(s) of exportable class could not "
                        "get a Jolt body (cMaxBodies capacity exhausted) - raise cMaxBodies (docs "
                        "§22.14/§35)", ctx.skippedCapacityCount);
        }
        LOG_INFO("Jolt: exported %d static obstacles (%d box, %d sphere, %d trimesh, %d capsule, "
                 "%d resting prop(s) [docs §55], %d tagged breakable [docs §57]) - %d geoms walked "
                 "total (%d nested sub-space(s) recursed into, %d at top level per "
                 "dSpaceGetNumGeoms), %d live-vehicle geoms skipped, %d body-less geoms of "
                 "unexported class skipped, %d skipped (capacity exhausted)",
                 exportedCount, ctx.boxCount, ctx.sphereCount, ctx.triMeshCount, ctx.capsuleCount,
                 ctx.propCount, ctx.taggedBreakableCount, ctx.walkedCount, ctx.nestedSpaceCount,
                 topLevelCount, ctx.skippedDynamicCount, ctx.skippedOtherClassCount,
                 ctx.skippedCapacityCount);
        // docs §59/§61: funnel breakdown, see StaticExportContext's own comment - breakableNotDestroyable
        // dropped (docs §61: no longer a rejection reason, trees are tagged too now).
        LOG_INFO("docs §59: untagged resting-prop breakdown - noGeomData=%d notPhysicBody=%d "
                 "noOwner=%d ownerNotBreakable=%d (sum should equal %d resting minus %d tagged = %d)",
                 ctx.unresolvedNoData, ctx.unresolvedNotPhysicBody, ctx.unresolvedNoOwner,
                 ctx.unresolvedNotBreakable,
                 ctx.propCount, ctx.taggedBreakableCount, ctx.propCount - ctx.taggedBreakableCount);
        for (int32_t c = 0; c < 32; ++c) {
            const bool isSpaceClass = c >= dFirstSpaceClass && c <= dLastSpaceClass;
            if (ctx.classHistogram[c] > 0 && c != dBoxClass && c != dSphereClass && c != dTriMeshClass
                && c != dCCylinderClass && c != dGeomTransformClass && !isSpaceClass) {
                LOG_INFO("Jolt: static obstacle export - %d geom(s) of unexported ODE class %d "
                         "(docs §22.9/§22.13/§55 - classes confirmed by disassembly: 0=sphere, 1=box, "
                         "2=capsule, 6=transform (unwrapped, not \"unexported\"), 7=trimesh, "
                         "11-14=space (recursed into, not \"unexported\"); others not confirmed "
                         "against this binary, treat the raw ID as a lead not a fact)",
                         ctx.classHistogram[c], c);
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

        // docs §54 (Этап 1, шаг -1E): 16MB -> 48MB, raised BEFORE/with cMaxContactConstraints
        // below, not after. JPH::TempAllocatorImpl::Allocate has no graceful fallback - it
        // std::abort()s the process when the linear buffer is exhausted - and PhysicsSystem's
        // per-step constraint buffer is sized mMaxConstraints * cMaxConstraintSize (~472 bytes on
        // 32-bit), so raising the constraint cap without raising this first turns a capacity
        // overflow from "dropped contacts" into an instant hard abort.
        g_tempAllocator = new JPH::TempAllocatorImpl(48 * 1024 * 1024);

        uint32_t threads = config.jolt_threads.value;
        if (threads == 0) {
            uint32_t hw = std::thread::hardware_concurrency();
            threads = hw > 1 ? hw - 1 : 1;
        }
        g_jobSystem = new JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, (int) threads);

        g_broadPhaseLayerInterface = new BPLayerInterfaceImpl();
        g_objectVsBroadPhaseFilter = new ObjectVsBroadPhaseLayerFilterImpl();
        g_objectLayerPairFilter    = new ObjectLayerPairFilterImpl();

        // cMaxBodies bumped again (docs §35) - 65536 (itself already a bump from 8192, docs
        // §22.14, which was itself a bump from an original 1024 placeholder) turned out to be a
        // real, live-confirmed truncation bug AGAIN, on a different, bigger real game save
        // ("Molokovoz", mountain terrain): exported only 65516 of the level's static obstacles
        // before hitting the cap, logging 21566 "out of bodies" errors for the rest - true
        // candidate count on that level alone was 87082, nearly 3x the ~32861 that motivated the
        // previous bump. Two real levels in a row have both exceeded whatever the current cap
        // was sized for, so this time the headroom is sized generously above the larger of the
        // two measured levels rather than just barely over it. Live-reconfirmed after this bump:
        // same save now exports all 87082 with 0 capacity failures.
        // docs §54 (Этап 1, шаг -1E): raised together with the TempAllocator above, ahead of
        // giving every wheel its own body+constraint (~380 extra bodies / 380 extra SixDOF
        // constraints on a 76-vehicle scene). These two are NOT "bodies" limits - they bound the
        // per-step contact workload, which is what actually grows when wheels stop being raycasts
        // and start being real colliding bodies. Overflowing either is silent: Jolt reports it via
        // the EPhysicsUpdateError return value of Update() (now logged, see StepPhysics) and just
        // DROPS the excess contacts - i.e. the symptom is "vehicles occasionally sink through the
        // ground in a crowd" with nothing in the log, the same failure class as the cMaxBodies
        // truncation bugs above (§22.14/§35) that twice went unnoticed for a whole session.
        constexpr JPH::uint cMaxBodies            = 262144;
        constexpr JPH::uint cNumBodyMutexes        = 0; // 0 = Jolt default
        constexpr JPH::uint cMaxBodyPairs          = 65536;
        constexpr JPH::uint cMaxContactConstraints = 16384;

        g_physicsSystem = new JPH::PhysicsSystem();
        g_physicsSystem->Init(
            cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
            *g_broadPhaseLayerInterface, *g_objectVsBroadPhaseFilter, *g_objectLayerPairFilter);

        // docs §112 (Этап 1, шаг 8): mStepListenersBatchSize, the one step-8 lever with headroom
        // left. Jolt hands step listeners to workers in batches of this size (default 8); with 75
        // shadows each owning a VehicleStepListener that is ~10 batches, so the batch size sets how
        // finely that work can spread across cores. The plan says "8 -> 1-2 (замерить обе)".
        //
        // Deliberately a global knob and not a per-shadow one: it is a PhysicsSettings field, it
        // affects EVERY step listener in the world, and the plan flags that explicitly.
        {
            JPH::PhysicsSettings settings = g_physicsSystem->GetPhysicsSettings();
            const uint32_t batch = kraken::Config::Instance().jolt_step_listener_batch.value;
            if (batch > 0) {
                settings.mStepListenersBatchSize = (int) batch;
                g_physicsSystem->SetPhysicsSettings(settings);
            }
            LOG_INFO("Jolt: mStepListenersBatchSize=%d (config %u, 0 = leave Jolt's default)",
                g_physicsSystem->GetPhysicsSettings().mStepListenersBatchSize, batch);
        }

        LOG_INFO("Jolt Physics initialized (threads=%u, maxBodies=%u)", threads, cMaxBodies);

        routines::ChangeCall((void*) 0x005CA413, &PostServersLoadHook);
        routines::ChangeCall((void*) 0x005CAC68, &ReadRoadsFromXmlFileHook);
        LOG_INFO("Jolt: static geometry export hooks installed (landscape heightfield + roads + "
                 "static obstacles via gGlobalSpace walk)");
    }
}
