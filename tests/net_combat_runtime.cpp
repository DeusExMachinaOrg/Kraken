#include "net/combat_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace kraken::net;
using namespace kraken::net::combat_runtime;

namespace {

int failures = 0;
std::uint8_t fake_call_opcode = 0;
std::int32_t fake_call_displacement = 0;
bool fake_call_readable = false;

bool read_fake_callsite(const std::uintptr_t, std::uint8_t& opcode,
                        std::int32_t& displacement)
{
    if (!fake_call_readable)
        return false;
    opcode = fake_call_opcode;
    displacement = fake_call_displacement;
    return true;
}

void check(const bool condition, const char* message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

NetEntityRef identity(const NetId id = 7, const EntityGeneration generation = 3)
{
    return {id, generation};
}

ResourceCue cue(const char* name)
{
    return make_resource_cue(name);
}

struct FakeResolver final : ReplicaResolver {
    ReplicaBinding binding{identity(), 0x1001, 0x2002,
                           reinterpret_cast<void*>(0x1234), 42, true, true};
    int resolve_calls = 0;
    int retire_calls = 0;
    bool resolve_enabled = true;

    bool resolve(const NetEntityRef& requested, ReplicaBinding& output) override
    {
        ++resolve_calls;
        if (!resolve_enabled || requested.net_id != binding.identity.net_id ||
            requested.generation != binding.identity.generation)
            return false;
        output = binding;
        return true;
    }

    bool retire(const NetEntityRef& requested) override
    {
        if (requested.net_id != binding.identity.net_id ||
            requested.generation != binding.identity.generation)
            return false;
        ++retire_calls;
        return true;
    }
};

struct FakeNative {
    float health = 100.0f;
    float maximum = 100.0f;
    std::uint32_t flags = 0x4000u;
    int unsafe_sets = 0;
    int forbidden_damage_callbacks = 0;
    int forbidden_death_callbacks = 0;
    int effects = 0;
    int decals = 0;
    int removals = 0;
    bool disappeared = false;
    bool effect_result = true;
    bool decal_result = true;
    VehicleVector3 effect_position{};
    VehicleQuaternion effect_rotation{};
    float effect_scale = 0.0f;
    bool effect_remove_if_free = false;
    VehicleVector3 decal_position{};
    VehicleVector3 decal_normal{};
    VehicleVector3 decal_tangent{};
    MeshIdentity decal_mesh = 0;
    std::string decal_cue_name{};

    ReplicaNativeOperations operations()
    {
        ReplicaNativeOperations value{};
        value.get_health = [this](void*) { return health; };
        value.get_max_health = [this](void*) { return maximum; };
        value.set_health_unsafe = [this](void*, const float next) {
            ++unsafe_sets;
            health = next;
            return true;
        };
        value.read_flags = [this](void*) { return flags; };
        value.set_flags_masked = [this](void*, const std::uint32_t mask,
                                         const std::uint32_t bits) {
            flags = (flags & ~mask) | (bits & mask);
            return true;
        };
        value.create_effect_node = [this](void*, const ImpactPresentation& event,
                                          const ImpactGeometry& geometry) {
            ++effects;
            effect_position = geometry.effect_position;
            effect_rotation = event.effect_rotation;
            effect_scale = geometry.effect_scale;
            effect_remove_if_free = geometry.remove_if_free;
            return effect_result;
        };
        value.add_decal = [this](void*, const ImpactPresentation& event,
                                 const ImpactGeometry& geometry) {
            ++decals;
            decal_position = geometry.hit_position;
            decal_normal = geometry.contact_normal;
            decal_tangent = geometry.decal_tangent;
            decal_mesh = geometry.mesh_id;
            decal_cue_name = event.decal_cue.name;
            return decal_result;
        };
        value.schedule_removal = [this](void*) {
            ++removals;
            return true;
        };
        value.has_disappeared = [this](void*) { return disappeared; };
        return value;
    }
};

ImpactPresentation impact(const std::uint64_t event_id = 11,
                          const bool effect = true, const bool decal = false)
{
    ImpactPresentation value{};
    value.session_epoch = 9;
    value.event_id = event_id;
    value.server_tick = 100;
    value.shot_id = 4;
    value.shooter = identity(2, 1);
    value.gun = {8, 0x88};
    value.target.kind = ImpactTargetKind::DynamicEntity;
    value.target.dynamic = identity();
    value.target_part = {3, 0x33};
    value.surface = SurfaceKind::Metal;
    if (effect)
        value.effect_cue = cue("effects/impact/metal");
    if (decal)
        value.decal_cue = cue("decals/impact/metal");
    value.hit_position = {1.0f, 2.0f, 3.0f};
    value.effect_position = {4.0f, 5.0f, 6.0f};
    value.contact_normal = {0.0f, 1.0f, 0.0f};
    value.decal_tangent = decal ? VehicleVector3{1.0f, 0.0f, 0.0f}
                                : VehicleVector3{};
    value.has_decal_tangent = decal;
    value.effect_rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    value.effect_scale = 1.0f;
    value.blocked_reason = ImpactBlockedReason::Occluded;
    return value;
}

DamageResult damage(const std::uint64_t event_id, const std::uint64_t impact_id,
                    const float post_health, const bool dead = false)
{
    DamageResult value{};
    value.session_epoch = 9;
    value.event_id = event_id;
    value.server_tick = 101;
    value.shot_id = 4;
    value.impact_event_id = impact_id;
    value.shooter = identity(2, 1);
    value.target = identity();
    value.damage = 100.0f - post_health;
    value.post_health = post_health;
    value.dead_transition = dead;
    return value;
}

DeathWreckPresentation death()
{
    DeathWreckPresentation value{};
    value.session_epoch = 9;
    value.transition_id = 20;
    value.server_tick = 102;
    value.entity = identity();
    value.wreck_entity = identity(107, 1);
    value.death_cue = cue("effects/death/vehicle");
    value.wreck_cue = cue("wrecks/vehicle/default");
    value.wreck_archive_id = 77;
    value.wreck_archive_revision = 1;
    value.wreck_archive_digest = 0xabcdef0123456789ull;
    value.wreck_archive_size = 8192;
    value.wreck_archive_chunk_count = 8;
    value.wreck_archive_chunk_size = 1024;
    value.wreck_variant_id = 3;
    return value;
}

NativeObjectArchiveV2 wreck_archive()
{
    NativeObjectArchiveV2 value{};
    value.map_namespace = "efa/r1m1";
    value.resource_fingerprint = "rfp2-sha256:test";
    const std::string xml =
        "<Vehicle Prototype=\"vehicle/prototype\"><Runtime>"
        "<Visibility enabled=\"1\"/></Runtime></Vehicle>";
    value.canonical_xml.assign(reinterpret_cast<const Byte*>(xml.data()),
                               reinterpret_cast<const Byte*>(xml.data() + xml.size()));
    const auto add_manifest = [&value](const char* path, const char* parent,
                                       const char* resource,
                                       const char* prototype,
                                       const NativeObjectArchiveResourceKind kind) {
        NativeObjectArchiveManifestEntry entry{
            path, parent, resource, prototype, kind, 0};
        entry.resource_fingerprint =
            native_object_archive_resource_identity_digest(
                value.resource_fingerprint, entry.archive_path,
                entry.resource_name, entry.prototype_name);
        value.manifest.push_back(std::move(entry));
    };
    add_manifest("Vehicle", "", "", "vehicle/prototype",
                 NativeObjectArchiveResourceKind::Prototype);
    add_manifest("Vehicle/Runtime", "Vehicle", "", "",
                 NativeObjectArchiveResourceKind::Prototype);
    add_manifest("Vehicle/Runtime/Visibility", "Vehicle/Runtime", "", "",
                 NativeObjectArchiveResourceKind::Prototype);
    value.visual_runtime.push_back({"Vehicle/Runtime/Visibility",
                                    NativeObjectArchiveVisualRuntimeKind::Visibility,
                                    {}, {}, 0, true});
    std::vector<Byte> encoded;
    if (encode_native_object_archive_v2(value, encoded) !=
            NativeObjectArchiveErrorCode::None ||
        decode_native_object_archive_v2(ByteView{encoded}, value) !=
            NativeObjectArchiveErrorCode::None)
        return {};
    return value;
}

bool install_archive(ReplicaCombatRuntime& runtime,
                      const NativeObjectArchiveV2& archive,
                      DeathWreckPresentation& event)
{
    std::vector<Byte> encoded;
    if (encode_native_object_archive_v2(archive, encoded) !=
            NativeObjectArchiveErrorCode::None)
        return false;
    std::vector<NativeObjectArchiveChunk> chunks;
    if (make_native_object_archive_chunks(encoded, event.wreck_archive_id,
                                          event.wreck_archive_revision,
                                          archive.digest, chunks) !=
            NativeObjectArchiveTransferResult::Accepted)
        return false;
    event.wreck_archive_digest = archive.digest;
    event.wreck_archive_size = static_cast<std::uint32_t>(encoded.size());
    event.wreck_archive_chunk_count = static_cast<std::uint16_t>(chunks.size());
    event.wreck_archive_chunk_size =
        static_cast<std::uint16_t>(kNativeObjectArchiveChunkPayloadBytes);
    // The public runtime accepts the encoded chunk wire payload, not the
    // decoded value. Exercise the same bounded transfer path as production.
    for (const NativeObjectArchiveChunk& chunk : chunks) {
        std::vector<Byte> payload;
        if (encode_native_object_archive_chunk(chunk, payload) !=
                NativeObjectArchiveErrorCode::None ||
            !runtime.accept_wreck_archive_chunk(ByteView{payload}, 100))
            return false;
    }
    return true;
}

void test_transactional_wreck_materializer_order_and_rollback()
{
    std::vector<std::string> order;
    bool allow_pose = true;
    bool allow_retire = true;
    std::string failure_stage;
    int destroy_calls = 0;
    int unbind_calls = 0;
    std::vector<ObjId> queued_removals;
    WreckMaterializerOperations operations{};
    operations.create_suspended_transaction = [&order, &failure_stage](
        const NativeObjectArchiveV2&, const ReplicaBinding&,
        WreckMaterializerOperations::Transaction& transaction) {
        order.push_back("suspended_creation");
        if (failure_stage == "create")
            return static_cast<void*>(nullptr);
        transaction.created_object_ids.push_back(5001);
        return reinterpret_cast<void*>(0x5000);
    };
    operations.collect_created_objects = [](void*,
                                             WreckMaterializerOperations::Transaction& transaction) {
        if (transaction.created_object_ids.size() == 1) {
            transaction.created_object_ids.push_back(5002);
            transaction.created_object_ids.push_back(5003);
        }
    };
    operations.validate_resources = [&order, &failure_stage](
        void*, const NativeObjectArchiveV2&, const ReplicaBinding&) {
        order.push_back("resources");
        return failure_stage != "resources";
    };
    operations.load_structure = [&order, &failure_stage](void*, ByteView) {
        order.push_back("load_from_xml_structure");
        return failure_stage != "load";
    };
    operations.post_load_graph = [&order, &failure_stage](void*) {
        order.push_back("post_load_graph");
        return failure_stage != "post_load";
    };
    operations.create_visual_part = [&order, &failure_stage](void*) {
        order.push_back("create_visual_part");
        return failure_stage != "visual_part";
    };
    operations.apply_visual_runtime = [&order, &failure_stage](void*,
                                               const NativeObjectArchiveVisualRuntime&) {
        order.push_back("visual_runtime");
        return failure_stage != "visual_runtime";
    };
    operations.apply_authoritative_pose = [&order, &allow_pose,
                                           &failure_stage](void*,
                                                                  const CombatPose&) {
        order.push_back("authoritative_pose");
        return allow_pose && failure_stage != "pose";
    };
    operations.disable_physics = [&order, &failure_stage](void*) {
        order.push_back("disable_ode_physics");
        return failure_stage != "physics";
    };
    operations.remove_from_simulation = [&order, &failure_stage](void*) {
        order.push_back("remove_from_simulation");
        return failure_stage != "simulation";
    };
    operations.validate_wreck = [&order, &failure_stage](void*) {
        order.push_back("validate_wreck");
        return failure_stage != "validate";
    };
    operations.bind_wreck = [&order, &failure_stage](const NetEntityRef&, void*) {
        order.push_back("bind_wreck_identity");
        return failure_stage != "bind";
    };
    operations.retire_source = [&order, &allow_retire, &failure_stage](const NetEntityRef&) {
        order.push_back("retire_source");
        return allow_retire && failure_stage != "retire";
    };
    operations.unbind_wreck = [&order, &unbind_calls](const NetEntityRef&, void*) {
        order.push_back("unbind_wreck_identity");
        ++unbind_calls;
    };
    operations.destroy_transaction = [&order, &destroy_calls, &queued_removals](
        WreckMaterializerOperations::Transaction& transaction) {
        order.push_back("destroy_transaction");
        ++destroy_calls;
        for (auto iterator = transaction.created_object_ids.rbegin();
             iterator != transaction.created_object_ids.rend(); ++iterator)
            queued_removals.push_back(*iterator);
        transaction.created_object_ids.clear();
    };

    FakeResolver resolver;
    const ReplicaBinding source = resolver.binding;
    TransactionalWreckMaterializer materializer(std::move(operations));
    const NetEntityRef wreck = identity(99, 1);
    const CombatPose pose{{1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    check(materializer.materialize(wreck_archive(), source.identity, wreck,
                                    source, pose) ==
              WreckMaterializationResult::Committed,
          "wreck materializer commits after complete lifecycle");
    const std::vector<std::string> expected{
        "suspended_creation", "resources", "load_from_xml_structure",
        "post_load_graph", "create_visual_part", "visual_runtime",
        "authoritative_pose", "disable_ode_physics", "remove_from_simulation",
        "validate_wreck", "bind_wreck_identity", "retire_source"};
    check(order == expected, "wreck materializer callback order is atomic");
    check(unbind_calls == 0 && destroy_calls == 0,
          "committed wreck is not rolled back");

    allow_pose = false;
    order.clear();
    check(materializer.materialize(wreck_archive(), source.identity, wreck,
                                   source, pose) ==
              WreckMaterializationResult::PoseRejected,
          "pose failure rejects wreck before source retirement");
    check(std::find(order.begin(), order.end(), "retire_source") == order.end(),
          "pose failure preserves source binding");
    check(destroy_calls == 1, "pose failure destroys the full transaction");
    check(queued_removals == std::vector<ObjId>{5003, 5002, 5001},
          "rollback queues every created object in reverse order");

    allow_pose = true;
    allow_retire = false;
    order.clear();
    check(materializer.materialize(wreck_archive(), source.identity, wreck,
                                   source, pose) ==
              WreckMaterializationResult::SourceRetireRejected,
          "source retirement failure rolls back bound wreck");
    check(unbind_calls == 1 && destroy_calls == 2,
          "source retirement failure unbinds and destroys full wreck graph");

    allow_retire = true;
    const std::vector<std::string> failure_stages{
        "create", "resources", "load", "post_load", "visual_part",
        "visual_runtime", "pose", "physics", "simulation", "validate",
        "bind", "retire"};
    const int destroys_before_matrix = destroy_calls;
    for (const std::string& stage : failure_stages) {
        failure_stage = stage;
        allow_pose = true;
        order.clear();
        check(materializer.materialize(wreck_archive(), source.identity, wreck,
                                       source, pose) !=
                  WreckMaterializationResult::Committed,
              "injected materializer stage failure rolls back");
        check(std::find(order.begin(), order.end(), "retire_source") ==
                  order.end() || stage == "retire",
              "failed wreck stage preserves source retirement boundary");
    }
    check(destroy_calls >= destroys_before_matrix +
              static_cast<int>(failure_stages.size()) - 1,
          "every post-create injected failure destroys its transaction");
}

void test_invalid_archive_creates_no_native_objects()
{
    FakeResolver resolver;
    WreckMaterializerOperations operations{};
    int create_calls = 0;
    operations.create_suspended_transaction =
        [&create_calls](const NativeObjectArchiveV2&, const ReplicaBinding&,
                        WreckMaterializerOperations::Transaction&) {
            ++create_calls;
            return reinterpret_cast<void*>(0x5000);
        };
    TransactionalWreckMaterializer materializer(std::move(operations));
    NativeObjectArchiveV2 invalid = wreck_archive();
    invalid.manifest.clear();
    const CombatPose pose{{}, {0.0f, 0.0f, 0.0f, 1.0f}};
    check(materializer.materialize(invalid, resolver.binding.identity,
                                   identity(99, 1), resolver.binding, pose) ==
              WreckMaterializationResult::InvalidArchive,
          "invalid archive is rejected before native creation");
    check(create_calls == 0,
          "invalid archive creates zero transaction objects");
}

struct FakeHorn {
    int created = 0;
    int set_properties = 0;
    int added = 0;
    int removed = 0;
    int released = 0;

    HornNodeOperations operations()
    {
        HornNodeOperations value{};
        value.create_node = [this](void*, const ResourceCue&) {
            ++created;
            return reinterpret_cast<void*>(0x2000 + created);
        };
        value.set_property = [this](void*, const std::uint32_t property,
                                    const bool loop) {
            ++set_properties;
            return property == kHornLoopProperty && loop;
        };
        value.add_child = [this](void*, void*) { ++added; return true; };
        value.remove_child = [this](void*, void*) { ++removed; return true; };
        value.release_node = [this](void*) { ++released; };
        return value;
    }
};

void test_host_capture_branches_and_tls()
{
    CanonicalCueResolver resolver(
        [](const std::string_view name, ResourceCue& output) {
            output = make_resource_cue(name);
            return true;
        });
    HostImpactCapture capture(resolver);
    HostImpactObservation observation{};
    observation.session_epoch = 9;
    observation.event_id = 1;
    observation.server_tick = 10;
    observation.shot_id = 2;
    observation.shooter = identity(2, 1);
    observation.gun = {8, 0x88};
    observation.geometry.target_part = {3, 0x33};
    observation.target.kind = ImpactTargetKind::DynamicEntity;
    observation.target.dynamic = identity();
    observation.target_captured = true;
    observation.contact_captured = true;
    observation.geometry.effect_rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    observation.geometry.effect_position = {4.0f, 5.0f, 6.0f};
    observation.geometry.decal_tangent = {1.0f, 0.0f, 0.0f};
    observation.geometry.has_decal_tangent = true;
    observation.blocked_reason = ImpactBlockedReason::Occluded;
    {
        HostImpactCaptureScope outer(observation);
        check(outer.outermost(), "outer impact scope is outermost");
        record_effect_branch("effects/impact/metal");
        record_effect_geometry({4.0f, 5.0f, 6.0f},
                               {0.0f, 0.0f, 0.0f, 1.0f}, true, 1.25f);
        record_effect_produced();
        {
            HostImpactCaptureScope nested(observation);
            check(!nested.outermost(), "nested impact scope is reentrant");
            record_decal_branch("decals/impact/metal");
            record_decal_geometry({1.0f, 2.0f, 3.0f},
                                  {0.0f, 1.0f, 0.0f},
                                  {1.0f, 0.0f, 0.0f}, 0xfeed1234,
                                  {3, 0x33});
            record_decal_produced();
        }
        check(current_host_impact_capture() == &observation,
              "TLS impact context survives nested scope");
    }
    check(current_host_impact_capture() == nullptr,
          "TLS impact context is cleared");

    int impacts = 0;
    int damages = 0;
    CombatEventId last_damage_impact = 0xffffu;
    std::vector<const char*> publication_order;
    HostImpactPublication publication{
        [&impacts, &publication_order](const ImpactPresentation& value) {
            ++impacts;
            publication_order.push_back("impact");
            return value.effect_cue.name == "effects/impact/metal" &&
                   value.decal_cue.name == "decals/impact/metal" &&
                   value.effect_position.x == 4.0f &&
                   value.effect_scale == 1.25f && value.remove_if_free &&
                   value.decal_tangent.x == 1.0f &&
                   value.mesh_id == 0xfeed1234;
        },
        [&damages, &last_damage_impact, &publication_order](
            const DamageResult& value) {
            ++damages;
            last_damage_impact = value.impact_event_id;
            publication_order.push_back("damage");
            return true;
        }};
    check(capture.publish(observation, publication) ==
              RuntimeApplyResult::Applied,
          "host publishes actual effect/decal branches");
    check(impacts == 1 && damages == 0,
          "no-damage impact publishes only presentation");

    HostImpactObservation visual_and_damage = observation;
    visual_and_damage.event_id = 2;
    visual_and_damage.did_damage = true;
    visual_and_damage.blocked_reason = ImpactBlockedReason::None;
    visual_and_damage.damage = damage(2, 2, 75.0f);
    visual_and_damage.damage.shot_id = visual_and_damage.shot_id;
    const RuntimeApplyResult visual_damage_result =
        capture.publish(visual_and_damage, publication);
    check(visual_damage_result == RuntimeApplyResult::Applied,
          "visual/damage publication succeeds");
    check(last_damage_impact == 2,
          "correlated visual/damage publication keeps event correlation");
    check(publication_order.size() == 3 && publication_order[1] == "impact" &&
              publication_order[2] == "damage",
          "visual is published before correlated authoritative damage");

    HostImpactObservation damage_only{};
    damage_only.session_epoch = 9;
    damage_only.event_id = 4;
    damage_only.server_tick = 12;
    damage_only.shot_id = 5;
    damage_only.shooter = identity(2, 1);
    damage_only.did_damage = true;
    damage_only.damage = damage(4, 0xdead, 75.0f);
    damage_only.damage.shot_id = damage_only.shot_id;
    damage_only.target_captured = false;
    damage_only.contact_captured = false;
    const RuntimeApplyResult damage_only_result =
        capture.publish(damage_only, publication);
    check(damage_only_result == RuntimeApplyResult::Applied,
          "damage-only publication succeeds");
    check(impacts == 2 && damages == 2 && last_damage_impact == 0,
          "authoritative damage publishes without captured visual FX");

    HostImpactObservation invalid = observation;
    invalid.event_id = 3;
    invalid.effect_name = "../guessed";
    invalid.decal_produced = false;
    check(capture.publish(invalid, publication) == RuntimeApplyResult::InvalidEvent,
          "unresolved canonical cue fails closed");
}

void test_replica_impact_damage_and_policy()
{
    FakeResolver resolver;
    FakeNative native;
    ReplicaCombatRuntime runtime(resolver, native.operations());

    check(runtime.apply_impact(impact(11, true, false)) ==
              RuntimeApplyResult::Applied,
          "effect-only impact applies");
    check(native.effects == 1 && native.decals == 0,
          "effect-only calls only effect seam");
    check(native.effect_position.x == 4.0f && native.effect_position.z == 6.0f &&
              native.effect_scale == 1.0f && !native.effect_remove_if_free,
          "effect seam receives exact world position and transform controls");
    check(runtime.apply_impact(impact(12, false, true)) ==
              RuntimeApplyResult::Applied,
          "decal-only impact applies");
    check(native.decals == 1, "decal-only calls decal seam");
    check(native.decal_position.x == 1.0f && native.decal_normal.y == 1.0f &&
              native.decal_tangent.x == 1.0f && native.decal_mesh == 0 &&
              native.decal_cue_name == "decals/impact/metal",
          "decal seam receives ordered position normal tangent mesh and cue");
    check(runtime.apply_impact(impact(12, true, false)) ==
              RuntimeApplyResult::Duplicate,
          "duplicate impact is rejected");
    check(runtime.apply_impact(impact(10, true, false)) ==
              RuntimeApplyResult::Stale,
          "stale impact is rejected");

    check(runtime.apply_damage(damage(30, 11, 45.0f)) ==
              RuntimeApplyResult::Applied,
          "authoritative post-health applies");
    check(native.health == 45.0f && native.unsafe_sets == 1,
          "replica uses direct unsafe projection");
    check(native.forbidden_damage_callbacks == 0 &&
              native.forbidden_death_callbacks == 0,
          "replica never invokes native damage/death callbacks");
    check(runtime.apply_damage(damage(31, 11, 40.0f)) ==
              RuntimeApplyResult::Duplicate,
          "same impact correlation cannot be replayed as damage");

    check(runtime.apply_damage(damage(32, 0, 35.0f)) ==
              RuntimeApplyResult::Applied,
          "authoritative damage without visual correlation applies");
    check(runtime.apply_damage(damage(32, 0, 30.0f)) ==
              RuntimeApplyResult::Duplicate,
          "duplicate zero-reference damage is rejected");
    check(runtime.apply_damage(damage(33, 0, 25.0f)) ==
              RuntimeApplyResult::Applied,
          "distinct zero-reference damage remains correlatable by event");
    check(runtime.apply_damage(damage(34, 999, 20.0f)) ==
              RuntimeApplyResult::InvalidEvent,
          "damage with unseen visual correlation is rejected");

    DamageResult invalid = damage(35, 12, 200.0f);
    invalid.damage = 0.0f;
    check(runtime.apply_damage(invalid) == RuntimeApplyResult::InvalidHealth,
          "out-of-range health fails closed");
    invalid = damage(36, 12, std::numeric_limits<float>::quiet_NaN());
    check(runtime.apply_damage(invalid) == RuntimeApplyResult::InvalidEvent,
          "non-finite health fails closed");
    invalid = damage(37, 0, 50.0f);
    invalid.target.generation = kInvalidEntityGeneration;
    check(runtime.apply_damage(invalid) == RuntimeApplyResult::InvalidEvent,
          "damage with invalid target identity fails closed");
    invalid = damage(38, 0, 50.0f);
    invalid.shot_id = 0;
    check(runtime.apply_damage(invalid) == RuntimeApplyResult::InvalidEvent,
          "damage with invalid shot identity fails closed");

    ImpactPresentation environment = impact(20, true, false);
    environment.target = {ImpactTargetKind::Environment, {}, {},
                          EnvironmentKind::Terrain};
    environment.target_part = {};
    check(runtime.apply_impact(environment) == RuntimeApplyResult::Applied,
          "world-space terrain effect applies without object identity");
    ImpactPresentation environment_decal = environment;
    environment_decal.event_id = 21;
    environment_decal.decal_cue = cue("decals/impact/terrain");
    environment_decal.has_decal_tangent = true;
    environment_decal.decal_tangent = {1.0f, 0.0f, 0.0f};
    check(runtime.apply_impact(environment_decal) ==
              RuntimeApplyResult::NativeRejected,
          "environment decal remains fail-closed without static binding");
}

void test_dead_projection_and_wreck_gate()
{
    FakeResolver resolver;
    FakeNative native;
    const NativeObjectArchiveV2 archive = wreck_archive();
    bool wreck_ready = false;
    ReplicaCombatRuntime runtime(
        resolver, native.operations(), {},
        [&wreck_ready, &archive](const DeathWreckPresentation&, const ReplicaBinding&,
                       WreckResolution& output) {
            output.inert_replacement_ready = wreck_ready;
            output.archive_verified = wreck_ready;
            output.archive_digest = archive.digest;
            output.wreck_identity = {107, 1};
            return true;
        });
    check(runtime.apply_impact(impact(50)) == RuntimeApplyResult::Applied,
          "death setup impact applies");
    check(runtime.apply_damage(damage(51, 50, 0.0f, true)) ==
              RuntimeApplyResult::Applied,
          "dead transition uses validated health");
    check((native.flags & kObjDeadFlag) != 0,
          "dead bit is projected by masked write");
    check((native.flags & ~kObjDeadFlag) == 0x4000u,
          "masked dead write preserves unrelated flags");

    wreck_ready = false;
    DeathWreckPresentation pending = death();
    check(install_archive(runtime, archive, pending),
          "complete archive transfer is accepted before death linkage");
    check(runtime.apply_death(pending) == RuntimeApplyResult::PendingRemoval,
          "death waits for wreck resolver");
    check(native.removals == 0, "unready wreck is not removed");
    wreck_ready = true;
    check(runtime.process_removals(false) == RuntimeApplyResult::PendingRemoval,
          "removal waits for pre-sim boundary");
    check(runtime.process_removals(true) == RuntimeApplyResult::PendingRemoval,
          "pre-sim schedules but waits for disappearance");
    check(native.removals == 1, "only confirmed wreck is scheduled");
    native.disappeared = true;
    check(runtime.process_removals(true) == RuntimeApplyResult::Applied,
          "identity retires after disappearance");
    check(resolver.retire_calls >= 1, "replica identity retires after removal");

    FakeResolver pre_scheduled_resolver;
    FakeNative pre_scheduled_native;
    pre_scheduled_native.health = 0.0f;
    ReplicaCombatRuntime pre_scheduled_runtime(
        pre_scheduled_resolver, pre_scheduled_native.operations(), {},
        [&archive](const DeathWreckPresentation&, const ReplicaBinding&,
                   WreckResolution& output) {
            output.inert_replacement_ready = true;
            output.archive_verified = true;
            output.archive_digest = archive.digest;
            output.wreck_identity = {107, 1};
            output.source_removal_scheduled = true;
            return true;
        });
    check(install_archive(pre_scheduled_runtime, archive, pending),
          "pre-scheduled removal setup archive is accepted");
    check(pre_scheduled_runtime.apply_death(pending) ==
              RuntimeApplyResult::PendingRemoval,
          "pre-scheduled source death enters the pre-sim barrier");
    check(pre_scheduled_runtime.process_removals(true) ==
              RuntimeApplyResult::PendingRemoval,
          "pre-scheduled source still waits for disappearance");
    check(pre_scheduled_native.removals == 0,
          "materializer-scheduled source is not queued for removal twice");
    pre_scheduled_native.disappeared = true;
    check(pre_scheduled_runtime.process_removals(true) ==
              RuntimeApplyResult::Applied,
          "pre-scheduled source retires after disappearance");
}

void test_horn_and_jip_cleanup()
{
    FakeResolver resolver;
    FakeNative native;
    FakeHorn horn;
    ReplicaCombatRuntime runtime(resolver, native.operations(), horn.operations());

    HornState state{};
    state.session_epoch = 9;
    state.transition_id = 1;
    state.server_tick = 1;
    state.vehicle = identity();
    state.active = true;
    state.horn_cue = cue("sounds/vehicle/horn");
    check(runtime.apply_horn(state) == RuntimeApplyResult::Applied,
          "horn starts through sidecar");
    check(runtime.apply_horn(state) == RuntimeApplyResult::Duplicate,
          "horn start is idempotent");
    check(horn.created == 1 && horn.added == 1 && horn.set_properties == 1,
          "horn has one looped node");
    state.active = false;
    state.transition_id = 2;
    check(runtime.apply_horn(state) == RuntimeApplyResult::Applied,
          "horn stops through sidecar");
    check(horn.removed == 1 && horn.released == 1,
          "horn node is removed and released");

    state.active = true;
    state.transition_id = 3;
    state.horn_cue = {};
    check(runtime.apply_horn(state) == RuntimeApplyResult::InvalidCue,
          "active horn without canonical cue fails closed");

    state.horn_cue = cue("sounds/vehicle/horn");
    check(runtime.apply_horn(state) == RuntimeApplyResult::Applied,
          "horn restarts after stop");
    runtime.despawn(identity());
    check(horn.removed == 2 && horn.released == 2,
          "despawn cleans horn sidecar");

    state.active = true;
    state.transition_id = 4;
    PresentationJipState jip{};
    jip.session_epoch = 9;
    jip.state_revision = 1;
    jip.chunk_count = 1;
    jip.horn_states.push_back(state);
    check(runtime.apply_jip(jip) == RuntimeApplyResult::Applied,
          "JIP horn state applies");
    runtime.reset();
    check(horn.removed == 3 && horn.released == 3,
          "session reset cleans JIP horn node");
}

void test_invalid_identity_and_hook_once()
{
    FakeResolver resolver;
    FakeNative native;
    ReplicaCombatRuntime runtime(resolver, native.operations());
    ImpactPresentation invalid = impact(100);
    invalid.target.dynamic.generation = kInvalidEntityGeneration;
    check(runtime.apply_impact(invalid) == RuntimeApplyResult::InvalidEvent,
          "invalid generation is rejected");
    resolver.resolve_enabled = false;
    check(runtime.apply_impact(impact(101)) == RuntimeApplyResult::InvalidIdentity,
          "unresolved replica identity fails closed");

    HostProcessShellAndBodyHook hook;
    int installs = 0;
    HostProcessHookInstaller installer{
        [&installs](const std::uintptr_t address,
                    const ProcessShellAndBodyFunction replacement,
                    ProcessShellAndBodyFunction& original) {
            ++installs;
            check(address == kProcessShellAndBodyVa,
                  "hook uses exact ProcessShellAndBody VA");
            check(replacement != nullptr, "hook replacement is present");
            original = reinterpret_cast<ProcessShellAndBodyFunction>(0x607200);
            return true;
        }};
    const ProcessShellAndBodyFunction replacement =
        &process_shell_and_body_capture_hook;
    check(hook.install(installer, replacement), "host process hook installs");
    check(hook.install(installer, replacement), "hook install is idempotent");
    check(installs == 1, "ProcessShellAndBody is wrapped exactly once");
}

void test_callsite_preflight_fail_closed()
{
    check(kVehicleUpdateVa == 0x005EC0D0u &&
              kVehicleEvaluateToDeadCallSiteVa == 0x005EC1D9u &&
              kVehicleEvaluateToDeadVa == 0x005E88E0u,
          "wreck hook uses the audited Vehicle::Update call and target VAs");
    constexpr std::uintptr_t call_site = 0x1000u;
    constexpr std::uintptr_t original_target = 0x1200u;
    const auto expected_displacement = static_cast<std::int32_t>(
        original_target - call_site - 5u);
    const CallsiteExpectation expectation{call_site, original_target};
    fake_call_readable = true;
    fake_call_opcode = 0xE8u;
    fake_call_displacement = expected_displacement;
    check(preflight_rel32_call(expectation, &read_fake_callsite),
          "verified E8 callsite and original target pass preflight");
    fake_call_opcode = 0x90u;
    check(!preflight_rel32_call(expectation, &read_fake_callsite),
          "altered call opcode fails closed");
    fake_call_opcode = 0xE8u;
    ++fake_call_displacement;
    check(!preflight_rel32_call(expectation, &read_fake_callsite),
          "altered decoded call target fails closed");
    fake_call_readable = false;
    check(!preflight_rel32_call(expectation, &read_fake_callsite),
          "unreadable callsite fails closed");
}

void test_replica_source_policy_regression()
{
    const std::filesystem::path test_path(__FILE__);
    const std::vector<std::filesystem::path> candidates{
        test_path.parent_path().parent_path() / "source" / "net" /
            "combat_runtime.cpp",
        std::filesystem::current_path() / ".." / "source" / "net" /
            "combat_runtime.cpp",
        std::filesystem::current_path() / "source" / "net" /
            "combat_runtime.cpp"};
    std::string source;
    for (const auto& candidate : candidates) {
        std::ifstream input(candidate, std::ios::binary);
        if (input) {
            source.assign(std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>());
            break;
        }
    }
    check(!source.empty(), "combat runtime source is available for policy guard");
    for (const char* forbidden : {"Vehicle::InflictDamage",
                                  "_EvaluateToDead", "_SetDeadStatus",
                                  "BreakModel"})
        check(source.find(forbidden) == std::string::npos,
              "replica adapter contains no native damage/death callback");
    for (const char* required : {
             "damage.impact_event_id = visual_published ? observation.event_id : 0",
             "m_damage_events",
             "event.impact_event_id != 0",
             "opcode != 0xE8u",
             "decoded_target"})
        check(source.find(required) != std::string::npos,
              "replica damage dedup keeps zero-reference events separate");
}

void test_runtime_dispatch_integration_source()
{
#ifdef KRAKEN_RUNTIME_SOURCE_PATH
    std::ifstream input(KRAKEN_RUNTIME_SOURCE_PATH, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
#else
    const std::string source;
#endif
    check(!source.empty(), "runtime integration source is available");
    for (const char* required : {
             "std::unique_ptr<RuntimeCombatBridge> combat_runtime",
             "combat_runtime::ReplicaCombatRuntime",
             "combat_runtime::HostImpactCapture",
             "g_state.combat_runtime->apply_impact",
             "g_state.combat_runtime->apply_damage",
             "g_state.combat_runtime->apply_death",
             "g_state.combat_runtime->apply_horn",
             "MessageType::CombatImpactPresentation",
             "MessageType::CombatDamageResult",
             "MessageType::CombatDeathWreckPresentation",
             "MessageType::CombatHornState",
             "process_shell_and_body_capture_hook",
             "send_combat_payload(peer, MessageType::CombatImpactPresentation",
             "send_combat_payload(peer, MessageType::CombatDamageResult",
             "MessageType::CombatDeathWreckPresentation,\n                                   Channel::Reliable",
             "send_combat_payload(peer, MessageType::CombatHornState",
             "process_removals(true)",
             "if (!g_state.is_host && !IsSessionActive())\n            apply_pending_impact_damage();",
             "combat_runtime::kNumericSetUnsafeVa",
             "combat_runtime::kObjDeadFlag",
             "combat_runtime::kCreateNodeVa",
             "combat_runtime::kSgNodeAddChildVa",
             "server->m_pObjects->AddObjIdToRemove",
             "resolve_process_body_target(body, actual_target, actual_part)",
             "reinterpret_cast<hta::m3d::Object*>(body)",
             "collider_is_kind(hta::ai::Wheel::p_classObject)",
             "collider_is_kind(hta::ai::Vehicle::p_classObject)",
             "reinterpret_cast<hta::ai::Vehicle*>(body)",
             "reinterpret_cast<hta::ai::Wheel*>(body)->GetVehicle()",
             "collider_is_kind(hta::ai::VehiclePart::p_classObject)",
             "std::array<const hta::ai::VehiclePart*, 256> visited",
             "visited[index] == current",
             "depth != visited.size()",
             "GetOwnerCompound",
             "kVehiclePartGetOwnerCompoundConstVa",
             "kPhysicBodyGetBaseClassVa",
             "collider_is_kind(physic_body_class)",
             "resolve_owner_vehicle(current->GetOwner())",
             "resolve_owner_vehicle(body->GetOwner())",
             "VehiclePart::p_classObject",
             "EnvironmentKind::UnboundStatic",
             "capture_process_contact(contacts, contact_count, observation)",
             "observation.geometry.contact_normal",
             "record_effect_geometry",
             "record_decal_geometry",
             "kLandscapeEffectCallSiteVa",
             "kVehiclePartAddDecalCallSiteVa"})
        check(source.find(required) != std::string::npos,
              "runtime wires the typed combat path");

    for (const char* required : {
             "kVehicleUpdateVa",
             "kVehicleEvaluateToDeadCallSiteVa",
             "kVehicleEvaluateToDeadVa",
             "bytes must be E8 02 C7 FF FF",
             "g_evaluate_to_dead_original(vehicle)",
             "process_host_wreck_candidates()",
             "capture_native_object_archive(",
             "publish_host_wreck_archive",
             "create_suspended_transaction",
             "created_object_ids.rbegin()",
             "EntityKind::Wreck",
             "source_removal_scheduled",
             "m_impl->replica->accept_wreck_archive_chunk",
             "publish_host_wreck_spawn"})
        check(source.find(required) != std::string::npos,
              "production wreck archive and transactional wiring is present");

    const std::size_t materializer_begin = source.find(
        "combat_runtime::WreckMaterializerOperations wreck_operations{}");
    const std::size_t materializer_end = source.find(
        "wreck_materializer = std::make_unique", materializer_begin);
    check(materializer_begin != std::string::npos &&
              materializer_end != std::string::npos &&
              materializer_end > materializer_begin,
          "production materializer operation boundary is available");
    if (materializer_begin != std::string::npos &&
        materializer_end != std::string::npos) {
        const std::string materializer = source.substr(
            materializer_begin, materializer_end - materializer_begin);
        for (const char* forbidden : {"_DeadActions", "BreakModel",
                                      "LoadRuntimeValues", "Obj::Remove",
                                      "_SetDeadStatus"})
            check(materializer.find(forbidden) == std::string::npos,
                  "replica wreck materializer has no forbidden gameplay callback");
    }

    check(source.find("command.target_entity_id") == std::string::npos ||
              source.find("observation.target = {ImpactTargetKind::DynamicEntity") ==
                  std::string::npos,
          "weapon command does not seed the actual impact target");
    check(source.find("geometry.hit_tangent") == std::string::npos &&
              source.find("geometry.hit_normal") == std::string::npos,
          "incoming direction is not conflated with decal tangent");
    check(source.find("kCreateEffectNodeCallSiteVa") != std::string::npos &&
              source.find("replacements[index]") != std::string::npos,
          "final CreateEffectNode callsite is registered");
    check(source.find("capture_landscape_effect_node_call") !=
                  std::string::npos &&
              source.find("kLandscapeEffectCallSiteVa") != std::string::npos,
          "landscape CreateEffectNode callsite is registered");
    check(source.find("capture_terrain_selector") != std::string::npos &&
              source.find("capture_road_selector") != std::string::npos &&
              source.find("capture_statics_selector") != std::string::npos &&
              source.find("capture_vehicle_selector") != std::string::npos,
          "all proven effect selector seams are wired");
    check(source.find("preflight_combat_branch_callsites") !=
                  std::string::npos &&
              source.find("preflight_rel32_call") != std::string::npos,
          "all combat branch callsites are preflighted as exact E8 calls");
    check(source.find("kVehiclePartEffectSelectorCallSiteVa") !=
                  std::string::npos &&
              source.find("replacements[index]") != std::string::npos,
          "VehiclePart selector callsite is included in the preflighted hook set");
    const std::size_t vehicle_selector = source.find(
        "const hta::CStr& __fastcall capture_vehicle_selector");
    const std::size_t vehicle_selector_end = source.find(
        "void __fastcall capture_decal_call", vehicle_selector);
    check(vehicle_selector != std::string::npos &&
              vehicle_selector_end != std::string::npos &&
              source.substr(vehicle_selector,
                            vehicle_selector_end - vehicle_selector)
                      .find("record_effect_produced") == std::string::npos,
          "selector observation alone does not claim VehiclePart FX production");
    check(source.find("else if (is_static)") == std::string::npos &&
              source.find("const bool is_static") == std::string::npos,
          "ProcessShellAndBody bool is not guessed as static classification");
    check(source.find("VehiclePart* const part") != std::string::npos &&
              source.find("original(part, nullptr, position, normal, tangent,") !=
                  std::string::npos,
          "decal wrapper preserves proven normal/tangent argument order");
    check(source.find("reinterpret_cast<AddDecal>(\n                    combat_runtime::kVehiclePartAddDecalVa)") !=
                  std::string::npos &&
              source.find("part->_AddDecal(") == std::string::npos,
          "replica decal binding uses the proven __fastcall seam");

    const std::size_t legacy_receive = source.rfind(
        "void receive_impact_damage(const SessionEvent& event)");
    check(legacy_receive != std::string::npos,
          "legacy decoder remains present only for compatibility");
    if (legacy_receive != std::string::npos) {
        const std::size_t legacy_guard = source.find(
            "g_state.is_host || IsSessionActive() ||", legacy_receive);
        check(legacy_guard != std::string::npos &&
                  legacy_guard < legacy_receive + 600,
              "legacy client receive path is rejected during multiplayer");
    }
    const std::size_t legacy_apply = source.find(
        "bool apply_impact_damage_result(const ImpactDamage& event)");
    check(legacy_apply != std::string::npos,
          "legacy apply function remains compatibility-only");
    if (legacy_apply != std::string::npos) {
        const std::size_t legacy_apply_guard = source.find(
            "g_state.is_host || IsSessionActive()", legacy_apply);
        check(legacy_apply_guard != std::string::npos,
              "legacy client native callback path is unreachable in session");
    }

    const std::size_t resolver_begin = source.find(
        "struct RuntimeCombatBridge::Impl");
    const std::size_t resolver_end = source.find("} resolver;", resolver_begin);
    check(resolver_begin != std::string::npos &&
              resolver_end != std::string::npos && resolver_end > resolver_begin,
          "production replica resolver source is available");
    if (resolver_begin != std::string::npos && resolver_end != std::string::npos) {
        const std::string resolver = source.substr(
            resolver_begin, resolver_end - resolver_begin);
        check(resolver.find("EntityKind::PlayerVehicle") != std::string::npos &&
                  resolver.find("EntityKind::NpcVehicle") != std::string::npos,
              "resolver admits player and NPC vehicle replicas");
        check(resolver.find("EntityKind::WorldObject") == std::string::npos &&
                  resolver.find("EntityKind::LootContainer") == std::string::npos,
              "resolver rejects world and loot replicas");
    }

    const std::size_t decal_begin = source.find("native.add_decal =");
    const std::size_t decal_end = source.find("native.schedule_removal =",
                                             decal_begin);
    check(decal_begin != std::string::npos && decal_end != std::string::npos &&
              decal_end > decal_begin,
          "production dynamic decal binding source is available");
    if (decal_begin != std::string::npos && decal_end != std::string::npos) {
        const std::string decal = source.substr(decal_begin,
                                                decal_end - decal_begin);
        check(decal.find("find_native_part_by_identity") != std::string::npos,
              "decal binding uses native stable part identity");
        check(decal.find("has_descriptor") == std::string::npos &&
                  decal.find("VehicleDescriptor") == std::string::npos &&
                  decal.find("resolve_descriptor_part") == std::string::npos,
                  "decal binding is independent of player descriptors");
    }

    const std::size_t registration_begin = source.find(
        "HostEntity* register_host_entity(hta::ai::Vehicle& vehicle, EntityKind kind)");
    const std::size_t registration_end = source.find(
        "LootRecord* find_loot(LootId loot_id)", registration_begin);
    check(registration_begin != std::string::npos &&
              registration_end > registration_begin,
          "typed host entity registration source is available");
    if (registration_begin != std::string::npos &&
        registration_end > registration_begin) {
        const std::string registration = source.substr(
            registration_begin, registration_end - registration_begin);
        const std::size_t bind = registration.find("g_state.entities.bind");
        const std::size_t loadout = registration.find("capture_vehicle_loadout");
        const std::size_t insertion = registration.find(
            "g_state.host_entities.push_back");
        const std::size_t playing = registration.find(
            "g_state.match.state() == MatchState::Playing");
        const std::size_t barrier = registration.find(
            "g_state.world_journal.revision()");
        const std::size_t marker = registration.find(
            "KRAKEN_MP_ACCEPT native_entity_registered entity=%u generation=%u kind=%u barrierRevision=%llu");
        check(bind != std::string::npos && loadout != std::string::npos &&
                  insertion != std::string::npos && playing != std::string::npos &&
                  barrier != std::string::npos && marker != std::string::npos &&
                  bind < loadout && loadout < insertion && insertion < playing &&
                  playing < marker && marker < barrier,
              "typed NPC marker follows bind loadout insertion and Playing guard with an independent world barrier");
        check(registration.find("KRAKEN_MP_ACCEPT native_entity_created") ==
                  std::string::npos,
              "typed registration does not claim a generic ObjectCreated mutation");
    }

    const std::size_t world_observer_begin = source.find(
        "void observe_authoritative_world()\n{");
    const std::size_t world_observer_end = source.find(
        "void retire_network_vehicle(hta::ai::ObjContainer& objects,",
        world_observer_begin);
    check(world_observer_begin != std::string::npos &&
              world_observer_end > world_observer_begin,
          "generic world observer source is available");
    if (world_observer_begin != std::string::npos &&
        world_observer_end > world_observer_begin) {
        const std::string observer = source.substr(
            world_observer_begin, world_observer_end - world_observer_begin);
        check(observer.find("object_is_bound_vehicle_or_descendant(object)") !=
                      std::string::npos &&
                  observer.find("EntitySpawn/VehicleDescriptor") !=
                      std::string::npos,
              "generic observer continues excluding typed player and NPC vehicle trees");
        check(observer.find("KRAKEN_MP_ACCEPT native_entity_registered") ==
                      std::string::npos &&
                  observer.find("KRAKEN_MP_ACCEPT native_entity_created") ==
                      std::string::npos,
              "generic observer cannot emit a typed NPC acceptance marker");
    }

    const std::size_t arm_begin = source.find(
        "void run_combat_autotest_tick(const float elapsed_time)");
    const std::size_t arm_end = source.find(
        "void observe_authoritative_combat_autotest_death()", arm_begin);
    check(arm_begin != std::string::npos && arm_end > arm_begin,
          "combat acceptance arming source is available");
    if (arm_begin != std::string::npos && arm_end > arm_begin) {
        const std::string arm = source.substr(arm_begin, arm_end - arm_begin);
        const std::size_t entities = arm.find(
            "hta::ai::Vehicle* const shooter = find_vehicle(shooter_id)");
        const std::size_t attached = arm.find(
            "capture_weapon_identity(*shooter, weapon, validated_weapon_identity)");
        const std::size_t local_shooter = arm.find(
            "const bool local_is_shooter = host_shooter_scenario == g_state.is_host");
        const std::size_t can_fire = arm.find(
            "(local_is_shooter && !weapon->CanFire())");
        const std::size_t generations = arm.find(
            "g_state.entities.lookup_generation(shooter_id, shooter_generation)");
        const std::size_t marker = arm.find("KRAKEN_MP_ACCEPT combat_armed");
        check(entities != std::string::npos && attached != std::string::npos &&
                  local_shooter != std::string::npos &&
                  can_fire != std::string::npos &&
                  generations != std::string::npos &&
                  marker != std::string::npos && entities < attached &&
                  local_shooter < attached && attached < can_fire &&
                  can_fire < marker &&
                  generations < marker,
              "combat_armed requires attached-gun identity and gates native CanFire only for the local shooter");
        check(arm.find("if (!local_is_shooter)\n        return;", marker) !=
                  std::string::npos,
              "non-shooter arms without entering the firing path");
    }

    const std::size_t route_begin = source.find("void apply_deferred_route()\n{");
    const std::size_t confirm_begin = source.find(
        "void confirm_deferred_exit_route(hta::CMiracle3d* const application)",
        route_begin);
    check(route_begin != std::string::npos && confirm_begin > route_begin,
          "deferred route request and confirmation sources are available");
    if (route_begin != std::string::npos && confirm_begin > route_begin) {
        const std::string request = source.substr(
            route_begin, confirm_begin - route_begin);
        const std::size_t confirm_end = source.find(
            "void reset_match_state()", confirm_begin);
        const std::string confirmation = source.substr(
            confirm_begin, confirm_end - confirm_begin);
        check(request.find("StartMainMenu requested") != std::string::npos &&
                  request.find("route=main_menu result=success") ==
                      std::string::npos,
              "void StartMainMenu request cannot immediately claim route success");
        check(confirmation.find("GetCurGameMode() == hta::GS_MAINMENU") !=
                      std::string::npos &&
                  confirmation.find("GetCurGameMode() == hta::GS_GAME") !=
                      std::string::npos &&
                  confirmation.find("current_level_name() ==") !=
                      std::string::npos &&
                  confirmation.find("result=success") != std::string::npos,
              "session exit success requires later native mode/map confirmation");
    }

    const std::size_t survival_begin = source.find(
        "void emit_combat_host_survival_heartbeat()");
    const std::size_t survival_end = source.find(
        "int __fastcall controls_hook", survival_begin);
    check(survival_begin != std::string::npos && survival_end > survival_begin,
          "host survival heartbeat source is available");
    if (survival_begin != std::string::npos && survival_end > survival_begin) {
        const std::string survival = source.substr(
            survival_begin, survival_end - survival_begin);
        for (const char* required : {"g_state.combat_autotest_death_logged",
                                     "MatchState::Playing",
                                     "g_state.session->running()",
                                     "host->_GetDeadStatus()",
                                     "host->GetHealth() <= 0.0f",
                                     "KRAKEN_MP_ACCEPT combat_host_surviving"})
            check(survival.find(required) != std::string::npos,
                  "host survival marker is evidence-backed after client death");
    }
}

} // namespace

int main()
{
    test_host_capture_branches_and_tls();
    test_replica_impact_damage_and_policy();
    test_dead_projection_and_wreck_gate();
    test_transactional_wreck_materializer_order_and_rollback();
    test_invalid_archive_creates_no_native_objects();
    test_horn_and_jip_cleanup();
    test_invalid_identity_and_hook_once();
    test_callsite_preflight_fail_closed();
    test_replica_source_policy_regression();
    test_runtime_dispatch_integration_source();
    if (failures != 0) {
        std::cerr << failures << " combat runtime test(s) failed\n";
        return 1;
    }
    std::cout << "combat runtime tests passed\n";
    return 0;
}
