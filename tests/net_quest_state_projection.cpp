#include "net/quest_state_projection.hpp"
#include "net/wire_protocol.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace kraken::net;

#ifdef NDEBUG
#undef assert
#define assert(expression) do { if (!(expression)) { \
    std::cerr << "check failed: " << #expression << "\n"; std::exit(1); \
} } while (false)
#endif

namespace {

constexpr const char* kFingerprint = "sha256:quest-test";
constexpr const char* kMap = "maps/shared/test";

QuestProjectionIdentity make_identity(
    const char* path, const char* name, QuestProjectionSourceKind source_kind)
{
    QuestProjectionIdentity result;
    result.resource_fingerprint = kFingerprint;
    result.map_namespace = kMap;
    result.source_kind = source_kind;
    result.resource_path = path;
    result.stable_name = name;
    result.id = quest_projection_id_hash(result.canonical_key());
    return result;
}

QuestProjectionIdentity ref(const char* path, const char* name)
{ return make_identity(path, name, QuestProjectionSourceKind::ReferencedObject); }

QuestProjectionRecord trigger(const char* path, const char* name, std::int32_t count)
{
    QuestProjectionRecord result;
    result.identity = make_identity(path, name, QuestProjectionSourceKind::TriggerNormal);
    result.dependency_order = 3;
    TriggerProjectionState state;
    state.state = QuestTriggerState::Action;
    state.state_keep = true;
    state.count = count;
    state.timeout_for_time_period = 1.5f;
    state.frames_for_frames_passed = 17u;
    state.fly_path_for_cinematic_fly = "cinematic/path";
    state.id_for_cinema_msg = 12;
    state.object_refs = {ref("objects.xml", "vehicle_a"), ref("objects.xml", "vehicle_b")};
    state.call_event_id = 42;
    state.call_obj_name = "vehicle_a";
    state.call_obj_ref = state.object_refs.front();
    state.can_update = true;
    result.state = std::move(state);
    return result;
}

QuestProjectionRecord cinematic_trigger()
{
    QuestProjectionRecord result;
    result.identity = make_identity("triggers/cinema.xml", "cinema_one",
                                    QuestProjectionSourceKind::TriggerCinematic);
    result.state = TriggerProjectionState{};
    return result;
}

QuestProjectionRecord quest(const char* name, DynamicQuestStatus status)
{
    QuestProjectionRecord result;
    result.identity = make_identity("quest_states.xml", name,
                                    QuestProjectionSourceKind::DynamicQuest);
    result.dependency_order = 7;
    DynamicQuestProjectionState state;
    state.reward = 875;
    state.take_game_time = 0x123456789;
    state.status = status;
    state.hirer_name = "hirer_alpha";
    state.target_name = "target_beta";
    state.hirer_reference = ref("objects.xml", "hirer_alpha");
    state.target_reference = ref("objects.xml", "target_beta");
    result.state = std::move(state);
    return result;
}

void test_provenance_registry_and_requested_resolver()
{
    QuestTriggerProvenanceRegistry registry;
    assert(registry.bind(QuestProjectionSourceKind::TriggerNormal,
                         "maps/main_triggers.xml", "same_name") ==
           QuestTriggerProvenanceBindResult::Bound);
    const auto normal = registry.lookup("same_name");
    assert(normal.has_value() &&
           normal->source_kind == QuestProjectionSourceKind::TriggerNormal &&
           normal->resource_path == "maps/main_triggers.xml");
    assert(registry.bind(QuestProjectionSourceKind::TriggerCinematic,
                         "maps/cinema_triggers.xml", "same_name") ==
           QuestTriggerProvenanceBindResult::Duplicate);
    assert(registry.locked() && !registry.lookup("same_name").has_value());
    registry.reset();
    assert(!registry.lookup("same_name").has_value());
    assert(registry.bind(QuestProjectionSourceKind::TriggerCinematic,
                         "maps/cinema_triggers.xml", "same_name") ==
           QuestTriggerProvenanceBindResult::Bound);
    assert(registry.bind(QuestProjectionSourceKind::TriggerNormal,
                         "maps/main_triggers.xml", "other_name") ==
           QuestTriggerProvenanceBindResult::Bound);
    assert(registry.lookup("other_name")->source_kind ==
           QuestProjectionSourceKind::TriggerNormal);

    const auto normal_identity = make_identity(
        "maps/main_triggers.xml", "same_name", QuestProjectionSourceKind::TriggerNormal);
    const auto cinematic_identity = make_identity(
        "maps/cinema_triggers.xml", "same_name", QuestProjectionSourceKind::TriggerCinematic);
    assert(normal_identity.canonical_key() != cinematic_identity.canonical_key());
    // Runtime FlyPath/cinema-message changes are state fields, not provenance.
    TriggerProjectionState runtime_a;
    TriggerProjectionState runtime_b;
    runtime_a.fly_path_for_cinematic_fly = "changed_at_runtime";
    runtime_b.id_for_cinema_msg = 991;
    assert(runtime_a.fly_path_for_cinematic_fly != runtime_b.fly_path_for_cinematic_fly);
    assert(normal_identity == make_identity(
        "maps/main_triggers.xml", "same_name", QuestProjectionSourceKind::TriggerNormal));
    assert(!registry.lookup("missing_name").has_value());

    const auto shared = trigger("triggers/main.xml", "trigger_a", 5);
    const auto shared_quest = quest("quest_q", DynamicQuestStatus::Processing);
    const std::array<QuestProjectionRecord, 2> records{shared, shared_quest};
    std::vector<std::string> requested;
    assert(collect_quest_projection_reference_keys(records, requested));
    assert(requested.size() == 4);
    const std::vector<std::string> candidates{
        requested.front(), "unrelated|unnamed|candidate"};
    assert(resolve_quest_projection_reference(requested.front(), candidates) ==
           QuestProjectionReferenceResolution::Resolved);
    const std::vector<std::string> duplicate_candidates{
        requested.front(), requested.front()};
    assert(resolve_quest_projection_reference(requested.front(), duplicate_candidates) ==
           QuestProjectionReferenceResolution::Duplicate);
    assert(resolve_quest_projection_reference("missing", candidates) ==
           QuestProjectionReferenceResolution::Missing);
}

void test_provenance_map_lifetime_across_session_reset()
{
    QuestTriggerProvenanceRegistry registry;
    registry.begin_map_load(QuestProjectionSourceKind::TriggerNormal);
    assert(registry.bind(QuestProjectionSourceKind::TriggerNormal,
                         "maps/loaded/triggers.xml", "A") ==
           QuestTriggerProvenanceBindResult::Bound);
    assert(registry.bind(QuestProjectionSourceKind::TriggerNormal,
                         "maps/loaded/triggers.xml", "B") ==
           QuestTriggerProvenanceBindResult::Bound);

    // BeginSession/reset_match_state is a session reset, not a map reset.
    registry.preserve_session_reset();
    assert(registry.lookup("A").has_value());
    assert(registry.lookup("B").has_value());
    // This models same-map autoHost lookup after the session reset.
    assert(registry.lookup("A")->resource_path == "maps/loaded/triggers.xml");

    // A cinematic trigger load appends to the active map registry. A duplicate
    // request is rejected even if native creation would skip the object.
    assert(registry.bind(QuestProjectionSourceKind::TriggerCinematic,
                         "maps/loaded/cinema.xml", "C") ==
           QuestTriggerProvenanceBindResult::Bound);
    assert(registry.bind(QuestProjectionSourceKind::TriggerCinematic,
                         "maps/loaded/cinema.xml", "A") ==
           QuestTriggerProvenanceBindResult::Duplicate);
    assert(registry.locked());

    // The next normal map load starts a fresh namespace, then cinematic XML
    // appends to that new map.
    registry.begin_map_load(QuestProjectionSourceKind::TriggerNormal);
    assert(!registry.lookup("A").has_value());
    assert(!registry.lookup("B").has_value());
    assert(registry.bind(QuestProjectionSourceKind::TriggerCinematic,
                         "maps/next/cinema.xml", "C") ==
           QuestTriggerProvenanceBindResult::Bound);
    assert(registry.lookup("C")->resource_path == "maps/next/cinema.xml");
}

void test_tombstone_transaction_plan_and_full_rollback_shape()
{
    const auto full = trigger("triggers/main.xml", "trigger_a", 12);
    QuestProjectionTransactionPlan plan;
    const std::string key = full.identity.canonical_key();
    QuestProjectionRecord removed = full;
    removed.removed = true;
    const std::array<QuestProjectionRecord, 1> removed_records{removed};
    const std::array<std::string, 1> present{key};
    assert(build_quest_projection_transaction_plan(
               removed_records, present, plan) ==
           QuestProjectionTransactionPlanResult::TombstoneObjectPresent);
    assert(plan.apply_records.empty());
    const std::array<std::string, 0> absent{};
    assert(build_quest_projection_transaction_plan(
               removed_records, absent, plan) ==
           QuestProjectionTransactionPlanResult::Ready);
    assert(plan.apply_records.empty());
    const std::array<QuestProjectionRecord, 1> full_records{full};
    assert(build_quest_projection_transaction_plan(
               full_records, absent, plan) ==
           QuestProjectionTransactionPlanResult::Ready);
    assert(plan.apply_records.size() == 1 && plan.apply_records.front() == full);
    assert(quest_projection_tombstone_result(false) ==
           QuestProjectionTombstoneResult::NoOpAbsent);
    assert(quest_projection_tombstone_result(true) ==
           QuestProjectionTombstoneResult::RejectPresent);
}

void test_codec_identity_and_full_state()
{
    QuestProjectionSnapshot source;
    source.epoch = 11;
    source.revision = 4;
    source.resource_fingerprint = kFingerprint;
    source.records = {quest("quest_q", DynamicQuestStatus::Processing),
                      trigger("triggers/main.xml", "trigger_b", 9),
                      trigger("triggers/main.xml", "trigger_a", 4),
                      cinematic_trigger()};
    std::vector<Byte> first;
    std::vector<Byte> second;
    assert(encode_quest_projection_snapshot(source, first) ==
           QuestProjectionCodecError::None);
    QuestProjectionSnapshot decoded;
    assert(decode_quest_projection_snapshot(first, decoded) ==
           QuestProjectionCodecError::None);
    assert(decoded.resource_fingerprint == kFingerprint);
    assert(quest_projection_records_equal(decoded.records, source.records));
    assert(encode_quest_projection_snapshot(source, second) ==
           QuestProjectionCodecError::None);
    assert(first == second);

    std::reverse(source.records.begin(), source.records.end());
    std::vector<Byte> reordered;
    assert(encode_quest_projection_snapshot(source, reordered) ==
           QuestProjectionCodecError::None);
    assert(reordered == first);
    assert(make_identity("triggers/main.xml", "normal", QuestProjectionSourceKind::TriggerNormal)
               .canonical_key() !=
           make_identity("triggers/cinema.xml", "normal", QuestProjectionSourceKind::TriggerCinematic)
               .canonical_key());

    std::vector<Byte> malformed = first;
    malformed.pop_back();
    assert(decode_quest_projection_snapshot(malformed, decoded) !=
           QuestProjectionCodecError::None);
    malformed = first;
    malformed[4] = static_cast<Byte>(0xff);
    assert(decode_quest_projection_snapshot(malformed, decoded) ==
           QuestProjectionCodecError::BadVersion);

    QuestProjectionSnapshot duplicate = source;
    duplicate.records.push_back(duplicate.records.front());
    assert(encode_quest_projection_snapshot(duplicate, second) ==
           QuestProjectionCodecError::DuplicateIdentity);
    QuestProjectionSnapshot wrong_id = source;
    wrong_id.records.front().identity.id ^= 0x1234u;
    assert(encode_quest_projection_snapshot(wrong_id, second) ==
           QuestProjectionCodecError::IdentityHashMismatch);
    QuestProjectionSnapshot wrong_resource = source;
    wrong_resource.resource_fingerprint = "sha256:other";
    assert(encode_quest_projection_snapshot(wrong_resource, second) ==
           QuestProjectionCodecError::InvalidFingerprint);
    QuestProjectionSnapshot oversized = source;
    oversized.records.front().identity.resource_path.assign(
        kQuestProjectionMaxIdentityFieldBytes + 1, 'x');
    oversized.records.front().identity.id = quest_projection_id_hash(
        oversized.records.front().identity.canonical_key());
    assert(encode_quest_projection_snapshot(oversized, second) ==
           QuestProjectionCodecError::IdentityTooLong);
    assert(is_valid_message_type(MessageType::MatchQuestSnapshot));
    assert(requires_reliable_channel(MessageType::MatchQuestDelta));
}

void test_host_deltas_are_single_transition_and_bounded()
{
    QuestProjectionHost host(3);
    host.reset(22, kFingerprint);
    QuestProjectionDelta delta;
    assert(host.observe({}, delta) == QuestProjectionHostResult::Initialized);
    auto current = trigger("triggers/main.xml", "trigger_a", 1);
    assert(host.observe(std::span<const QuestProjectionRecord>(&current, 1), delta) ==
           QuestProjectionHostResult::DeltaProduced);
    assert(delta.resource_fingerprint == kFingerprint && delta.base_revision == 0 &&
           delta.revision == 1 && delta.records.size() == 1);
    assert(host.observe(std::span<const QuestProjectionRecord>(&current, 1), delta) ==
           QuestProjectionHostResult::Unchanged);
    current = trigger("triggers/main.xml", "trigger_a", 2);
    assert(host.observe(std::span<const QuestProjectionRecord>(&current, 1), delta) ==
           QuestProjectionHostResult::DeltaProduced);
    assert(delta.base_revision == 1 && delta.revision == 2);
    std::vector<QuestProjectionDelta> history;
    assert(host.deltas_after(22, 0, history) && history.size() == 2);
    assert(!host.deltas_after(21, 0, history));
}

void test_jip_ordering_gap_duplicate_epoch_and_transaction()
{
    std::vector<QuestProjectionRecord> applied;
    std::size_t apply_count = 0;
    bool fail = false;
    QuestProjectionClient client(4,
        [&applied, &apply_count, &fail](std::span<const QuestProjectionRecord> previous,
                                        std::span<const QuestProjectionRecord> target) {
            ++apply_count;
            (void)previous;
            if (fail)
                return false;
            applied.assign(target.begin(), target.end());
            return true;
        });
    const auto a = trigger("triggers/main.xml", "trigger_a", 1);
    const auto b = quest("quest_q", DynamicQuestStatus::Processing);
    QuestProjectionDelta d1{44, 0, 1, kFingerprint, {a}};
    QuestProjectionDelta d2{44, 1, 2, kFingerprint, {b}};
    assert(client.accept_delta(d2) == QuestProjectionClientResult::Buffered);
    assert(client.accept_delta(d1) == QuestProjectionClientResult::Buffered);
    assert(client.accept_delta(d1) == QuestProjectionClientResult::Duplicate);

    QuestProjectionSnapshot baseline{44, 0, kFingerprint, {}};
    assert(client.begin_snapshot(baseline) == QuestProjectionClientResult::BaselineAccepted);
    assert(!client.input_unlocked());
    assert(client.mark_world_ready() == QuestProjectionClientResult::BaselineAccepted);
    assert(client.commit() == QuestProjectionClientResult::Ready);
    assert(client.input_unlocked() && client.applied_revision() == 2 && applied.size() == 2);
    assert(apply_count == 1);

    fail = true;
    QuestProjectionDelta d3{44, 2, 3, kFingerprint,
                            {trigger("triggers/main.xml", "trigger_a", 99)}};
    assert(client.accept_delta(d3) == QuestProjectionClientResult::ApplyFailed);
    assert(client.resnapshot_required() && client.records().size() == 2);
    assert(applied.size() == 2);

    QuestProjectionClient wrong_resource;
    assert(wrong_resource.begin_snapshot(baseline) == QuestProjectionClientResult::BaselineAccepted);
    QuestProjectionRecord wrong_record = a;
    wrong_record.identity.resource_fingerprint = "sha256:other";
    wrong_record.identity.id = quest_projection_id_hash(
        wrong_record.identity.canonical_key());
    auto& wrong_state = std::get<TriggerProjectionState>(wrong_record.state);
    for (QuestProjectionIdentity& reference : wrong_state.object_refs) {
        reference.resource_fingerprint = "sha256:other";
        reference.id = quest_projection_id_hash(reference.canonical_key());
    }
    if (wrong_state.call_obj_ref.has_value()) {
        wrong_state.call_obj_ref->resource_fingerprint = "sha256:other";
        wrong_state.call_obj_ref->id = quest_projection_id_hash(
            wrong_state.call_obj_ref->canonical_key());
    }
    QuestProjectionDelta wrong_delta{44, 0, 1, "sha256:other", {wrong_record}};
    assert(wrong_resource.accept_delta(wrong_delta) ==
           QuestProjectionClientResult::WrongFingerprint);

    QuestProjectionClient gap;
    assert(gap.begin_snapshot(baseline) == QuestProjectionClientResult::BaselineAccepted);
    assert(gap.accept_delta(QuestProjectionDelta{44, 1, 2, kFingerprint, {a}}) ==
           QuestProjectionClientResult::Buffered);
    assert(gap.mark_world_ready() == QuestProjectionClientResult::BaselineAccepted);
    assert(gap.commit() == QuestProjectionClientResult::Gap);
    assert(gap.resnapshot_required());

    QuestProjectionClient epoch;
    assert(epoch.begin_snapshot(baseline) == QuestProjectionClientResult::BaselineAccepted);
    assert(epoch.accept_delta(QuestProjectionDelta{45, 0, 1, kFingerprint, {a}}) ==
           QuestProjectionClientResult::WrongEpoch);
}

void test_live_trigger_timer_progress_does_not_publish_revisions()
{
    QuestProjectionHost host(4);
    host.reset(31, kFingerprint);
    QuestProjectionDelta delta;
    QuestProjectionRecord current = trigger("triggers/main.xml", "timer", 1);
    auto& initial = std::get<TriggerProjectionState>(current.state);
    initial.timeout_for_time_period = 2.0f;
    initial.frames_for_frames_passed = 10;
    assert(host.observe(std::span<const QuestProjectionRecord>(&current, 1), delta) ==
           QuestProjectionHostResult::Initialized);

    QuestProjectionRecord progressed = current;
    auto& timer = std::get<TriggerProjectionState>(progressed.state);
    timer.timeout_for_time_period = 1.5f;
    timer.frames_for_frames_passed = 11;
    assert(quest_projection_live_records_equal(
        std::span<const QuestProjectionRecord>(&current, 1),
        std::span<const QuestProjectionRecord>(&progressed, 1)));
    assert(host.observe(std::span<const QuestProjectionRecord>(&progressed, 1), delta) ==
           QuestProjectionHostResult::Unchanged);
    assert(host.revision() == 0);
    const QuestProjectionSnapshot snapshot = host.snapshot();
    const auto& snapshot_timer =
        std::get<TriggerProjectionState>(snapshot.records.front().state);
    assert(snapshot_timer.timeout_for_time_period == 1.5f &&
           snapshot_timer.frames_for_frames_passed == 11);

    timer.count = 2;
    assert(!quest_projection_live_records_equal(
        std::span<const QuestProjectionRecord>(&current, 1),
        std::span<const QuestProjectionRecord>(&progressed, 1)));
    assert(host.observe(std::span<const QuestProjectionRecord>(&progressed, 1), delta) ==
           QuestProjectionHostResult::DeltaProduced);
    assert(delta.base_revision == 0 && delta.revision == 1);
}

void test_reset_overflow_replica_gate_and_personal_exclusion()
{
    QuestProjectionClient client(1);
    const auto a = trigger("triggers/main.xml", "a", 1);
    const auto b = trigger("triggers/main.xml", "b", 2);
    assert(client.accept_delta(QuestProjectionDelta{8, 0, 2, kFingerprint, {b}}) ==
           QuestProjectionClientResult::Buffered);
    assert(client.accept_delta(QuestProjectionDelta{8, 0, 3, kFingerprint, {a}}) ==
           QuestProjectionClientResult::Overflow);
    assert(client.resnapshot_required());
    client.reset();
    assert(client.state() == QuestProjectionClientState::Idle &&
           client.pending_count() == 0 && client.records().empty() &&
           client.resource_fingerprint().empty());

    // QuestStateManager/static personal profile records have no projection
    // variant and therefore cannot enter a shared wire snapshot.
    QuestProjectionSnapshot shared{9, 0, kFingerprint, {a}};
    for (const auto& record : shared.records)
        assert(record.kind() == QuestProjectionRecordKind::Trigger);

    for (const QuestReplicaPhase phase : {QuestReplicaPhase::Offline,
                                          QuestReplicaPhase::Loading,
                                          QuestReplicaPhase::Synchronizing,
                                          QuestReplicaPhase::Playing,
                                          QuestReplicaPhase::Teardown}) {
        const bool active_phase = phase == QuestReplicaPhase::Loading ||
                                  phase == QuestReplicaPhase::Synchronizing ||
                                  phase == QuestReplicaPhase::Playing;
        assert(quest_replica_execution_suppressed(false, true, phase) == active_phase);
        assert(!quest_replica_execution_suppressed(true, true, phase));
        assert(!quest_replica_execution_suppressed(false, false, phase));
    }

    std::vector<QuestProjectionRecord> bounded;
    bounded.reserve(64);
    for (std::size_t index = 0; index < 64; ++index)
        bounded.push_back(trigger("triggers/main.xml",
                                  (std::string("player_") + std::to_string(index)).c_str(),
                                  static_cast<std::int32_t>(index)));
    QuestProjectionSnapshot stress{99, 4, kFingerprint, bounded};
    std::vector<Byte> bytes;
    assert(encode_quest_projection_snapshot(stress, bytes) ==
           QuestProjectionCodecError::None);
    QuestProjectionSnapshot stress_decoded;
    assert(decode_quest_projection_snapshot(bytes, stress_decoded) ==
           QuestProjectionCodecError::None);
    assert(stress_decoded.records.size() == 64);
}

} // namespace

int main()
{
    test_provenance_registry_and_requested_resolver();
    test_provenance_map_lifetime_across_session_reset();
    test_tombstone_transaction_plan_and_full_rollback_shape();
    test_codec_identity_and_full_state();
    test_host_deltas_are_single_transition_and_bounded();
    test_jip_ordering_gap_duplicate_epoch_and_transaction();
    test_live_trigger_timer_progress_does_not_publish_revisions();
    test_reset_overflow_replica_gate_and_personal_exclusion();
    std::cout << "quest state projection tests passed\n";
    return 0;
}
