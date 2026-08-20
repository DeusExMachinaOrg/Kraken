#ifndef KRAKEN_NET_QUEST_STATE_PROJECTION_HPP
#define KRAKEN_NET_QUEST_STATE_PROJECTION_HPP

#include "net/net_types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace kraken::net {

// Quest projection is presentation/state replication. It is never a command
// path: receivers only assign already-proven runtime fields.
inline constexpr std::uint32_t kQuestProjectionWireMagic = 0x31505151u;
inline constexpr std::uint16_t kQuestProjectionWireVersion = 3;
inline constexpr std::uint16_t kQuestProjectionWireFlags = 0;
inline constexpr std::size_t kQuestProjectionWireHeaderSize = 40;
inline constexpr std::size_t kQuestProjectionMaxRecords = 4096;
inline constexpr std::size_t kQuestProjectionMaxPendingDeltas = 64;
inline constexpr std::size_t kQuestProjectionMaxIdentityFieldBytes = 128;
inline constexpr std::size_t kQuestProjectionMaxObjectReferences = 256;
inline constexpr std::size_t kQuestProjectionMaxWireBytes = 1024u * 1024u;

using QuestProjectionEpoch = std::uint32_t;
using QuestProjectionRevision = std::uint64_t;
using QuestProjectionId = std::uint64_t;

inline constexpr QuestProjectionEpoch kInvalidQuestProjectionEpoch = 0;
inline constexpr QuestProjectionRevision kInvalidQuestProjectionRevision = 0;
inline constexpr QuestProjectionId kInvalidQuestProjectionId = 0;

// Independent from world_authority's Loading policy. Offline/host sessions
// remain native while an active client replica is suppressed through all map
// phases until teardown.
enum class QuestReplicaPhase : std::uint8_t {
    Offline,
    Loading,
    Synchronizing,
    Playing,
    Teardown,
};

[[nodiscard]] constexpr bool quest_replica_execution_suppressed(
    bool is_host, bool replica_active, QuestReplicaPhase phase) noexcept
{
    return !is_host && replica_active &&
           (phase == QuestReplicaPhase::Loading ||
            phase == QuestReplicaPhase::Synchronizing ||
            phase == QuestReplicaPhase::Playing);
}

enum class QuestProjectionRecordKind : std::uint8_t {
    Trigger = 1,
    DynamicQuest = 2,
};

enum class QuestProjectionSourceKind : std::uint8_t {
    TriggerNormal = 1,
    TriggerCinematic = 2,
    DynamicQuest = 3,
    ReferencedObject = 4,
};

struct QuestTriggerProvenance final {
    QuestProjectionSourceKind source_kind = QuestProjectionSourceKind::TriggerNormal;
    std::string resource_path;
};

enum class QuestTriggerProvenanceBindResult : std::uint8_t {
    Bound,
    Invalid,
    Duplicate,
    Locked,
};

// Trigger XML provenance is captured at the two verified Load call sites. A
// name is deliberately unique for the active map: the same name in normal and
// cinematic XML is ambiguous and locks the registry until map/session reset.
class QuestTriggerProvenanceRegistry final {
public:
    [[nodiscard]] QuestTriggerProvenanceBindResult bind(
        QuestProjectionSourceKind source_kind,
        std::string resource_path,
        std::string stable_name);
    [[nodiscard]] std::optional<QuestTriggerProvenance> lookup(
        std::string_view stable_name) const;
    // Normal trigger loading starts a new map provenance namespace. Cinematic
    // loading appends to the already-loaded map namespace, while a session
    // reset deliberately preserves the map-lifetime registry.
    void begin_map_load(QuestProjectionSourceKind source_kind) noexcept;
    void preserve_session_reset() noexcept {}
    void reset() noexcept;
    [[nodiscard]] bool locked() const noexcept { return m_locked; }
    [[nodiscard]] std::size_t size() const noexcept { return m_entries.size(); }

private:
    struct Entry final {
        std::string stable_name;
        QuestTriggerProvenance provenance;
    };
    bool m_locked = false;
    std::vector<Entry> m_entries;
};

enum class QuestTriggerState : std::uint8_t {
    EventWait = 0,
    Action = 1,
    Off = 2,
};

enum class DynamicQuestStatus : std::uint8_t {
    NotTaken = 0,
    Processing = 1,
    Complete = 2,
    Failed = 3,
    Forgotten = 4,
};

// Every identity is explicit and stable across enumeration order, pointers,
// ObjIds, and Lua IDs. Trigger/quest names are bound to their source resource;
// referenced-object names are accepted only when unique in the active map.
// Fingerprint is repeated here in addition to the packet header so nested
// references are independently bound.
struct QuestProjectionIdentity {
    std::string resource_fingerprint;
    std::string map_namespace;
    QuestProjectionSourceKind source_kind = QuestProjectionSourceKind::ReferencedObject;
    std::string resource_path;
    std::string stable_name;
    QuestProjectionId id = kInvalidQuestProjectionId;

    [[nodiscard]] std::string canonical_key() const;
    [[nodiscard]] bool valid() const;
    [[nodiscard]] bool operator==(const QuestProjectionIdentity& other) const;
};

[[nodiscard]] QuestProjectionId quest_projection_id_hash(
    std::string_view canonical_key) noexcept;

struct TriggerProjectionState {
    QuestTriggerState state = QuestTriggerState::EventWait;
    bool state_keep = false;
    std::int32_t count = 0;
    float timeout_for_time_period = 0.0f;
    std::uint32_t frames_for_frames_passed = 0;
    std::string fly_path_for_cinematic_fly;
    std::int32_t id_for_cinema_msg = 0;
    std::vector<QuestProjectionIdentity> object_refs;
    std::int32_t call_event_id = 0;
    std::string call_obj_name;
    // Legacy-capable optional field. Native m_callObjId is the ephemeral
    // caller of the most recent event and current hosts intentionally omit it.
    std::optional<QuestProjectionIdentity> call_obj_ref;
    bool can_update = false;

    [[nodiscard]] bool operator==(const TriggerProjectionState&) const = default;
};

struct DynamicQuestProjectionState {
    std::int32_t reward = 0;
    std::int64_t take_game_time = 0;
    DynamicQuestStatus status = DynamicQuestStatus::NotTaken;
    std::string hirer_name;
    std::string target_name;
    std::optional<QuestProjectionIdentity> hirer_reference;
    std::optional<QuestProjectionIdentity> target_reference;

    [[nodiscard]] bool operator==(const DynamicQuestProjectionState&) const = default;
};

struct QuestProjectionRecord;

enum class QuestProjectionReferenceResolution : std::uint8_t {
    Missing,
    Resolved,
    Duplicate,
};

// These helpers are the production resolver contract used by the native
// application seam. Candidate keys are supplied only for requested refs;
// unrelated unnamed or duplicate objects never become a failure condition.
[[nodiscard]] bool collect_quest_projection_reference_keys(
    std::span<const QuestProjectionRecord> records,
    std::vector<std::string>& output);
[[nodiscard]] QuestProjectionReferenceResolution resolve_quest_projection_reference(
    std::string_view requested_key,
    std::span<const std::string> candidate_keys) noexcept;

enum class QuestProjectionTombstoneResult : std::uint8_t {
    NoOpAbsent,
    RejectPresent,
};

[[nodiscard]] constexpr QuestProjectionTombstoneResult
quest_projection_tombstone_result(bool object_present) noexcept
{ return object_present ? QuestProjectionTombstoneResult::RejectPresent
                        : QuestProjectionTombstoneResult::NoOpAbsent; }

struct QuestProjectionTransactionPlan final {
    std::vector<QuestProjectionRecord> apply_records;
};

enum class QuestProjectionTransactionPlanResult : std::uint8_t {
    Ready,
    InvalidRecord,
    TombstoneObjectPresent,
};

// Builds the no-side-effect portion of the native transaction. It preserves
// every scalar/reference field in non-removed records and never invents a
// terminal state for an omitted object.
[[nodiscard]] QuestProjectionTransactionPlanResult
build_quest_projection_transaction_plan(
    std::span<const QuestProjectionRecord> target,
    std::span<const std::string> locally_present_keys,
    QuestProjectionTransactionPlan& output);

struct QuestProjectionRecord {
    QuestProjectionIdentity identity;
    std::uint32_t dependency_order = 0;
    bool removed = false;
    std::variant<TriggerProjectionState, DynamicQuestProjectionState> state =
        TriggerProjectionState{};

    [[nodiscard]] QuestProjectionRecordKind kind() const noexcept;
    [[nodiscard]] bool operator==(const QuestProjectionRecord& other) const;
};

struct QuestProjectionSnapshot {
    QuestProjectionEpoch epoch = kInvalidQuestProjectionEpoch;
    QuestProjectionRevision revision = kInvalidQuestProjectionRevision;
    std::string resource_fingerprint;
    std::vector<QuestProjectionRecord> records;
};

struct QuestProjectionDelta {
    QuestProjectionEpoch epoch = kInvalidQuestProjectionEpoch;
    QuestProjectionRevision base_revision = kInvalidQuestProjectionRevision;
    QuestProjectionRevision revision = kInvalidQuestProjectionRevision;
    std::string resource_fingerprint;
    std::vector<QuestProjectionRecord> records;
};

enum class QuestProjectionCodecError : std::uint8_t {
    None,
    InputSizeMismatch,
    BadMagic,
    BadVersion,
    BadKind,
    BadFlags,
    WireTooLarge,
    PayloadTooLarge,
    TooManyRecords,
    TooManyReferences,
    InvalidEpoch,
    InvalidRevision,
    InvalidFingerprint,
    InvalidIdentity,
    IdentityTooLong,
    IdentityHashMismatch,
    DuplicateIdentity,
    HashCollision,
    InvalidRecordKind,
    InvalidSourceKind,
    InvalidTriggerState,
    InvalidQuestStatus,
    InvalidScalar,
    InvalidDeltaRange,
    RemovedInSnapshot,
};

[[nodiscard]] constexpr bool quest_projection_codec_succeeded(
    QuestProjectionCodecError error) noexcept
{ return error == QuestProjectionCodecError::None; }

[[nodiscard]] QuestProjectionCodecError encode_quest_projection_snapshot(
    const QuestProjectionSnapshot& snapshot, std::vector<Byte>& output);
[[nodiscard]] QuestProjectionCodecError decode_quest_projection_snapshot(
    ByteView input, QuestProjectionSnapshot& output);
[[nodiscard]] QuestProjectionCodecError encode_quest_projection_delta(
    const QuestProjectionDelta& delta, std::vector<Byte>& output);
[[nodiscard]] QuestProjectionCodecError decode_quest_projection_delta(
    ByteView input, QuestProjectionDelta& output);
[[nodiscard]] bool quest_projection_records_equal(
    std::span<const QuestProjectionRecord> left,
    std::span<const QuestProjectionRecord> right);
// Trigger timeout/frame counters are host-internal scheduling progress. They
// are retained in snapshots and meaningful deltas, but do not by themselves
// create a live network revision while client trigger execution is suppressed.
[[nodiscard]] bool quest_projection_live_records_equal(
    std::span<const QuestProjectionRecord> left,
    std::span<const QuestProjectionRecord> right);

using QuestProjectionApplyCallback = std::function<bool(
    std::span<const QuestProjectionRecord> previous,
    std::span<const QuestProjectionRecord> target)>;

enum class QuestProjectionHostResult : std::uint8_t {
    None,
    Initialized,
    Unchanged,
    DeltaProduced,
    InvalidInput,
    RevisionExhausted,
    HistoryOverflow,
};

class QuestProjectionHost final {
public:
    explicit QuestProjectionHost(
        std::size_t history_capacity = kQuestProjectionMaxPendingDeltas);

    void reset(QuestProjectionEpoch epoch = 1,
               std::string resource_fingerprint = {});
    void set_resource_fingerprint(std::string resource_fingerprint);

    [[nodiscard]] QuestProjectionEpoch epoch() const noexcept { return m_epoch; }
    [[nodiscard]] QuestProjectionRevision revision() const noexcept { return m_revision; }
    [[nodiscard]] bool initialized() const noexcept { return m_initialized; }
    [[nodiscard]] const std::string& resource_fingerprint() const noexcept
    { return m_resource_fingerprint; }
    [[nodiscard]] const std::vector<QuestProjectionRecord>& records() const noexcept
    { return m_records; }
    [[nodiscard]] QuestProjectionSnapshot snapshot() const;
    [[nodiscard]] QuestProjectionHostResult observe(
        std::span<const QuestProjectionRecord> records,
        QuestProjectionDelta& emitted);
    [[nodiscard]] bool deltas_after(
        QuestProjectionEpoch epoch, QuestProjectionRevision revision,
        std::vector<QuestProjectionDelta>& output) const;

private:
    std::size_t m_history_capacity = 0;
    QuestProjectionEpoch m_epoch = 1;
    QuestProjectionRevision m_revision = 0;
    bool m_initialized = false;
    std::string m_resource_fingerprint;
    std::vector<QuestProjectionRecord> m_records;
    std::vector<QuestProjectionDelta> m_history;
};

enum class QuestProjectionClientState : std::uint8_t {
    Idle,
    Transferring,
    Ready,
    ResnapshotRequired,
};

enum class QuestProjectionClientResult : std::uint8_t {
    None,
    Buffered,
    Duplicate,
    BaselineAccepted,
    Applied,
    Ready,
    WrongEpoch,
    WrongFingerprint,
    Gap,
    Overflow,
    Invalid,
    ApplyFailed,
    NotReady,
    ResnapshotRequired,
};

// JIP barrier: baseline and all newer deltas are validated and replayed as
// one transaction only after the world snapshot has committed.
class QuestProjectionClient final {
public:
    explicit QuestProjectionClient(
        std::size_t pending_capacity = kQuestProjectionMaxPendingDeltas,
        QuestProjectionApplyCallback applier = {});

    void reset() noexcept;
    void set_applier(QuestProjectionApplyCallback applier);
    [[nodiscard]] QuestProjectionClientState state() const noexcept { return m_state; }
    [[nodiscard]] bool ready() const noexcept
    { return m_state == QuestProjectionClientState::Ready; }
    [[nodiscard]] bool resnapshot_required() const noexcept
    { return m_state == QuestProjectionClientState::ResnapshotRequired; }
    [[nodiscard]] bool input_unlocked() const noexcept
    { return ready() && m_world_ready; }
    [[nodiscard]] QuestProjectionEpoch epoch() const noexcept { return m_epoch; }
    [[nodiscard]] QuestProjectionRevision snapshot_revision() const noexcept
    { return m_snapshot_revision; }
    [[nodiscard]] QuestProjectionRevision applied_revision() const noexcept
    { return m_applied_revision; }
    [[nodiscard]] const std::string& resource_fingerprint() const noexcept
    { return m_resource_fingerprint; }
    [[nodiscard]] std::size_t pending_count() const noexcept { return m_pending.size(); }
    [[nodiscard]] const std::vector<QuestProjectionRecord>& records() const noexcept
    { return m_records; }
    [[nodiscard]] QuestProjectionClientResult accept_delta(
        const QuestProjectionDelta& delta);
    [[nodiscard]] QuestProjectionClientResult begin_snapshot(
        const QuestProjectionSnapshot& snapshot);
    [[nodiscard]] QuestProjectionClientResult mark_world_ready() noexcept;
    [[nodiscard]] QuestProjectionClientResult commit();

private:
    [[nodiscard]] QuestProjectionClientResult fail_closed(
        QuestProjectionClientResult result) noexcept;
    [[nodiscard]] bool queue_delta(const QuestProjectionDelta& delta);
    [[nodiscard]] QuestProjectionClientResult apply_target(
        std::span<const QuestProjectionRecord> target,
        QuestProjectionRevision revision);
    [[nodiscard]] QuestProjectionClientResult replay_pending();

    std::size_t m_pending_capacity = 0;
    QuestProjectionClientState m_state = QuestProjectionClientState::Idle;
    QuestProjectionEpoch m_epoch = kInvalidQuestProjectionEpoch;
    QuestProjectionRevision m_snapshot_revision = kInvalidQuestProjectionRevision;
    QuestProjectionRevision m_applied_revision = kInvalidQuestProjectionRevision;
    bool m_world_ready = false;
    std::string m_resource_fingerprint;
    std::vector<QuestProjectionRecord> m_baseline;
    std::vector<QuestProjectionDelta> m_pending;
    std::vector<QuestProjectionRecord> m_records;
    QuestProjectionApplyCallback m_applier;
};

} // namespace kraken::net

#endif // KRAKEN_NET_QUEST_STATE_PROJECTION_HPP
