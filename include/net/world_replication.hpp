#ifndef KRAKEN_NET_WORLD_REPLICATION_HPP
#define KRAKEN_NET_WORLD_REPLICATION_HPP

#include "net/net_types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace kraken::net {

// The replication layer deliberately transports opaque payloads.  The
// entity, vehicle, and world-loot protocols own their payload formats; this
// module only provides the ordering and join barrier around them.
using WorldEpoch = std::uint32_t;
using WorldRevision = std::uint64_t;

inline constexpr WorldEpoch kInvalidWorldEpoch = 0;
inline constexpr WorldRevision kInvalidWorldRevision = 0;
inline constexpr std::size_t kDefaultWorldJournalCapacity = 64;
inline constexpr std::size_t kDefaultWorldJoinBufferCapacity = 64;

struct WorldDelta {
    WorldEpoch epoch = kInvalidWorldEpoch;
    WorldRevision revision = kInvalidWorldRevision;
    std::vector<Byte> payload;
};

struct WorldSnapshot {
    WorldEpoch epoch = kInvalidWorldEpoch;
    // Revision zero is the valid initial snapshot revision.  Mutations
    // published by WorldJournal always receive a nonzero revision.
    WorldRevision revision = kInvalidWorldRevision;
    std::vector<Byte> payload;
};

enum class WorldJournalResult : std::uint8_t {
    Appended,
    RevisionExhausted,
};

// A host-owned, bounded mutation journal.  Revisions are ordered within an
// epoch.  Once full, the oldest deltas are discarded; a peer whose snapshot
// predates the retained range must receive a fresh snapshot.
class WorldJournal final {
public:
    explicit WorldJournal(
        std::size_t capacity = kDefaultWorldJournalCapacity);

    [[nodiscard]] std::size_t capacity() const noexcept
    { return m_capacity; }
    [[nodiscard]] std::size_t size() const noexcept
    { return m_deltas.size(); }
    [[nodiscard]] bool empty() const noexcept
    { return m_deltas.empty(); }
    [[nodiscard]] bool full() const noexcept
    { return m_deltas.size() == m_capacity; }

    [[nodiscard]] WorldEpoch epoch() const noexcept
    { return m_epoch; }
    [[nodiscard]] WorldRevision revision() const noexcept
    { return m_revision; }

    // Appending an empty payload is valid: a caller may use a separate
    // protocol discriminator or carry an empty-world marker.  If the
    // revision counter is exhausted, the returned delta has revision zero;
    // try_append() should be used when that distinction matters.
    [[nodiscard]] WorldDelta append(ByteView payload);
    [[nodiscard]] WorldDelta append(const std::vector<Byte>& payload)
    { return append(ByteView{payload}); }
    [[nodiscard]] WorldDelta append(std::vector<Byte>&& payload);

    [[nodiscard]] std::optional<WorldDelta> try_append(ByteView payload);
    [[nodiscard]] std::optional<WorldDelta> try_append(
        const std::vector<Byte>& payload)
    { return try_append(ByteView{payload}); }

    // Starts a new epoch and clears retained deltas.  The epoch counter is
    // never allowed to become zero.  False is only possible at uint32 max.
    [[nodiscard]] bool reset_epoch() noexcept;
    [[nodiscard]] bool advance_epoch() noexcept
    { return reset_epoch(); }

    [[nodiscard]] const std::vector<WorldDelta>& deltas() const noexcept
    { return m_deltas; }

    // Copies every retained delta strictly newer than revision into output.
    // False means the requested range has already fallen out of the bounded
    // journal (or asks for a future revision).  The output is cleared first.
    [[nodiscard]] bool deltas_after(
        WorldEpoch requested_epoch, WorldRevision revision,
        std::vector<WorldDelta>& output) const;
    [[nodiscard]] bool can_replay_from(
        WorldEpoch requested_epoch, WorldRevision revision) const noexcept;

    [[nodiscard]] WorldSnapshot capture_snapshot() const
    { return capture_snapshot(ByteView{}); }
    [[nodiscard]] WorldSnapshot capture_snapshot(ByteView payload) const;
    [[nodiscard]] WorldSnapshot capture_snapshot(
        const std::vector<Byte>& payload) const
    { return capture_snapshot(ByteView{payload}); }
    [[nodiscard]] WorldSnapshot capture_snapshot(
        std::vector<Byte>&& payload) const;

    [[nodiscard]] WorldSnapshot make_snapshot(ByteView payload) const
    { return capture_snapshot(payload); }
    [[nodiscard]] WorldSnapshot make_snapshot() const
    { return capture_snapshot(); }

private:
    std::size_t m_capacity = 0;
    WorldEpoch m_epoch = 1;
    WorldRevision m_revision = 0;
    std::vector<WorldDelta> m_deltas;
};

enum class WorldJoinState : std::uint8_t {
    Idle,
    Transferring,
    Ready,
    ResnapshotRequired,
};

enum class WorldJoinResult : std::uint8_t {
    Started,
    Buffered,
    Applied,
    Ready,
    Duplicate,
    WrongEpoch,
    Gap,
    Overflow,
    InvalidDelta,
    InvalidSnapshot,
    NotTransferring,
    ResnapshotRequired,
    ApplyFailed,

    // Descriptive aliases keep call sites readable without multiplying
    // result handling branches.
    DuplicateDelta = Duplicate,
    WrongSessionEpoch = WrongEpoch,
    GapDetected = Gap,
    BufferOverflow = Overflow,
    Accepted = Started,
    Begun = Started,
};

[[nodiscard]] constexpr bool world_join_succeeded(
    WorldJoinResult result) noexcept
{
    return result == WorldJoinResult::Buffered ||
           result == WorldJoinResult::Applied ||
           result == WorldJoinResult::Ready ||
           result == WorldJoinResult::Started;
}

using WorldSnapshotApplyCallback = std::function<bool(ByteView)>;
using WorldDeltaApplyCallback = std::function<bool(const WorldDelta&)>;

// Coordinates a late join.  begin() establishes the epoch and the revision
// represented by the in-flight snapshot.  Deltas received while that
// snapshot is transferring are retained in arrival order, but only the next
// contiguous revision is accepted.  commit() invokes the snapshot callback
// first, then each buffered delta callback, and reaches Ready only after all
// callbacks succeed.
class WorldJoinBarrier final {
public:
    explicit WorldJoinBarrier(
        std::size_t max_buffered_deltas = kDefaultWorldJoinBufferCapacity,
        WorldSnapshotApplyCallback snapshot_applier = {},
        WorldDeltaApplyCallback delta_applier = {});

    [[nodiscard]] std::size_t capacity() const noexcept
    { return m_capacity; }
    [[nodiscard]] std::size_t buffered_count() const noexcept
    { return m_buffered.size(); }
    [[nodiscard]] bool empty() const noexcept
    { return m_buffered.empty(); }

    [[nodiscard]] WorldJoinState state() const noexcept
    { return m_state; }
    [[nodiscard]] bool ready() const noexcept
    { return m_state == WorldJoinState::Ready; }
    [[nodiscard]] bool resnapshot_required() const noexcept
    { return m_state == WorldJoinState::ResnapshotRequired; }

    [[nodiscard]] WorldEpoch epoch() const noexcept
    { return m_epoch; }
    [[nodiscard]] WorldRevision snapshot_revision() const noexcept
    { return m_snapshot_revision; }
    [[nodiscard]] WorldRevision applied_revision() const noexcept
    { return m_applied_revision; }
    [[nodiscard]] WorldRevision next_revision() const noexcept
    { return m_next_revision; }

    [[nodiscard]] const std::vector<WorldDelta>& buffered_deltas() const noexcept
    { return m_buffered; }

    [[nodiscard]] WorldJoinResult begin(
        WorldEpoch epoch, WorldRevision snapshot_revision) noexcept;
    [[nodiscard]] WorldJoinResult begin(const WorldSnapshot& snapshot) noexcept
    { return begin(snapshot.epoch, snapshot.revision); }
    [[nodiscard]] WorldJoinResult begin_snapshot(
        WorldEpoch epoch, WorldRevision snapshot_revision) noexcept
    { return begin(epoch, snapshot_revision); }

    [[nodiscard]] WorldJoinResult accept_delta(const WorldDelta& delta);
    [[nodiscard]] WorldJoinResult push_delta(const WorldDelta& delta)
    { return accept_delta(delta); }

    [[nodiscard]] WorldJoinResult commit(
        const WorldSnapshot& snapshot,
        WorldSnapshotApplyCallback snapshot_applier = {},
        WorldDeltaApplyCallback delta_applier = {});
    [[nodiscard]] WorldJoinResult commit_snapshot(
        const WorldSnapshot& snapshot,
        WorldSnapshotApplyCallback snapshot_applier = {},
        WorldDeltaApplyCallback delta_applier = {})
    { return commit(snapshot, std::move(snapshot_applier),
                    std::move(delta_applier)); }

    void reset() noexcept;

private:
    [[nodiscard]] WorldJoinResult reject_unusable_delta(
        const WorldDelta& delta) const noexcept;
    [[nodiscard]] bool apply_snapshot(const WorldSnapshot& snapshot);
    [[nodiscard]] bool apply_delta(const WorldDelta& delta);

    std::size_t m_capacity = 0;
    WorldJoinState m_state = WorldJoinState::Idle;
    WorldEpoch m_epoch = kInvalidWorldEpoch;
    WorldRevision m_snapshot_revision = kInvalidWorldRevision;
    WorldRevision m_applied_revision = kInvalidWorldRevision;
    WorldRevision m_next_revision = kInvalidWorldRevision;
    std::vector<WorldDelta> m_buffered;
    WorldSnapshotApplyCallback m_snapshot_applier;
    WorldDeltaApplyCallback m_delta_applier;
};

using WorldSnapshotJournal = WorldJournal;
using RevisionedWorldJournal = WorldJournal;
using WorldSnapshotBarrier = WorldJoinBarrier;
using WorldReplicationJoinBarrier = WorldJoinBarrier;

} // namespace kraken::net

#endif // KRAKEN_NET_WORLD_REPLICATION_HPP
