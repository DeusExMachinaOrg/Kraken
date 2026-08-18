#include "net/world_replication.hpp"

#include <limits>
#include <utility>

namespace kraken::net {

WorldJournal::WorldJournal(const std::size_t capacity)
    : m_capacity(capacity)
{
    // Keeping the vector reserved makes the retained range bounded and keeps
    // the usual append path from invalidating the reference returned by
    // deltas().  The capacity is caller-selected, so no hidden allocation
    // policy is imposed by the protocol.
    m_deltas.reserve(m_capacity);
}

WorldDelta WorldJournal::append(const ByteView payload)
{
    const std::optional<WorldDelta> result = try_append(payload);
    return result.has_value() ? *result : WorldDelta{m_epoch, 0, {}};
}

WorldDelta WorldJournal::append(std::vector<Byte>&& payload)
{
    if (m_revision == (std::numeric_limits<WorldRevision>::max)())
        return {m_epoch, kInvalidWorldRevision, {}};

    ++m_revision;
    WorldDelta delta{m_epoch, m_revision, std::move(payload)};
    if (m_capacity != 0) {
        if (m_deltas.size() == m_capacity)
            m_deltas.erase(m_deltas.begin());
        m_deltas.push_back(delta);
    }
    return delta;
}

std::optional<WorldDelta> WorldJournal::try_append(const ByteView payload)
{
    if (m_revision == (std::numeric_limits<WorldRevision>::max)())
        return std::nullopt;

    ++m_revision;
    WorldDelta delta{m_epoch,
                     m_revision,
                     std::vector<Byte>(payload.begin(), payload.end())};
    if (m_capacity != 0) {
        if (m_deltas.size() == m_capacity)
            m_deltas.erase(m_deltas.begin());
        m_deltas.push_back(delta);
    }
    return delta;
}

bool WorldJournal::reset_epoch() noexcept
{
    if (m_epoch == (std::numeric_limits<WorldEpoch>::max)())
        return false;

    ++m_epoch;
    m_revision = kInvalidWorldRevision;
    m_deltas.clear();
    return true;
}

bool WorldJournal::deltas_after(
    const WorldEpoch requested_epoch, const WorldRevision revision,
    std::vector<WorldDelta>& output) const
{
    output.clear();
    if (!can_replay_from(requested_epoch, revision))
        return false;

    for (const WorldDelta& delta : m_deltas)
        if (delta.revision > revision)
            output.push_back(delta);
    return true;
}

bool WorldJournal::can_replay_from(
    const WorldEpoch requested_epoch,
    const WorldRevision revision) const noexcept
{
    if (requested_epoch == kInvalidWorldEpoch || requested_epoch != m_epoch)
        return false;
    if (revision > m_revision)
        return false;
    if (revision == m_revision)
        return true;
    if (m_deltas.empty())
        return false;

    // The journal is contiguous by construction.  A snapshot at revision R
    // can be replayed when the first retained revision is no newer than R+1.
    return revision != (std::numeric_limits<WorldRevision>::max)() &&
           m_deltas.front().revision <= revision + 1;
}

WorldSnapshot WorldJournal::capture_snapshot(const ByteView payload) const
{
    return {m_epoch,
            m_revision,
            std::vector<Byte>(payload.begin(), payload.end())};
}

WorldSnapshot WorldJournal::capture_snapshot(std::vector<Byte>&& payload) const
{
    return {m_epoch, m_revision, std::move(payload)};
}

WorldJoinBarrier::WorldJoinBarrier(
    const std::size_t max_buffered_deltas,
    WorldSnapshotApplyCallback snapshot_applier,
    WorldDeltaApplyCallback delta_applier)
    : m_capacity(max_buffered_deltas),
      m_snapshot_applier(std::move(snapshot_applier)),
      m_delta_applier(std::move(delta_applier))
{
    m_buffered.reserve(m_capacity);
}

WorldJoinResult WorldJoinBarrier::begin(
    const WorldEpoch epoch, const WorldRevision snapshot_revision) noexcept
{
    if (epoch == kInvalidWorldEpoch)
        return WorldJoinResult::InvalidSnapshot;

    m_state = WorldJoinState::Transferring;
    m_epoch = epoch;
    m_snapshot_revision = snapshot_revision;
    m_applied_revision = kInvalidWorldRevision;
    m_next_revision =
        snapshot_revision == (std::numeric_limits<WorldRevision>::max)()
            ? kInvalidWorldRevision
            : snapshot_revision + 1;
    m_buffered.clear();
    return WorldJoinResult::Started;
}

WorldJoinResult WorldJoinBarrier::reject_unusable_delta(
    const WorldDelta& delta) const noexcept
{
    if (delta.epoch == kInvalidWorldEpoch ||
        delta.revision == kInvalidWorldRevision)
        return WorldJoinResult::InvalidDelta;
    if (delta.epoch != m_epoch)
        return WorldJoinResult::WrongEpoch;
    if (m_state == WorldJoinState::ResnapshotRequired)
        return WorldJoinResult::ResnapshotRequired;
    if (m_state == WorldJoinState::Idle)
        return WorldJoinResult::NotTransferring;
    return WorldJoinResult::Applied;
}

WorldJoinResult WorldJoinBarrier::accept_delta(const WorldDelta& delta)
{
    const WorldJoinResult preliminary = reject_unusable_delta(delta);
    if (preliminary != WorldJoinResult::Applied)
        return preliminary;

    // A revision below the next expected value was either already buffered,
    // already committed, or is older than the snapshot.  All are duplicates
    // from the barrier's perspective and must not be applied twice.
    if (delta.revision < m_next_revision)
        return WorldJoinResult::Duplicate;
    if (delta.revision > m_next_revision)
        return WorldJoinResult::Gap;

    if (m_state == WorldJoinState::Transferring) {
        if (m_buffered.size() == m_capacity) {
            m_buffered.clear();
            m_state = WorldJoinState::ResnapshotRequired;
            return WorldJoinResult::Overflow;
        }
        m_buffered.push_back(delta);
        ++m_next_revision;
        return WorldJoinResult::Buffered;
    }

    if (!apply_delta(delta)) {
        m_buffered.clear();
        m_state = WorldJoinState::ResnapshotRequired;
        return WorldJoinResult::ApplyFailed;
    }

    m_applied_revision = delta.revision;
    ++m_next_revision;
    return WorldJoinResult::Applied;
}

WorldJoinResult WorldJoinBarrier::commit(
    const WorldSnapshot& snapshot,
    WorldSnapshotApplyCallback snapshot_applier,
    WorldDeltaApplyCallback delta_applier)
{
    if (m_state == WorldJoinState::ResnapshotRequired)
        return WorldJoinResult::ResnapshotRequired;
    if (m_state != WorldJoinState::Transferring)
        return WorldJoinResult::NotTransferring;
    if (snapshot.epoch != m_epoch)
        return WorldJoinResult::WrongEpoch;
    if (snapshot.revision != m_snapshot_revision)
        return WorldJoinResult::InvalidSnapshot;

    // An empty callback means "keep the callback supplied at construction or
    // by an earlier commit".  This lets a caller commit a protocol-only
    // barrier while still retaining its live-delta applier.
    if (snapshot_applier)
        m_snapshot_applier = std::move(snapshot_applier);
    if (delta_applier)
        m_delta_applier = std::move(delta_applier);

    if (!apply_snapshot(snapshot)) {
        m_buffered.clear();
        m_state = WorldJoinState::ResnapshotRequired;
        return WorldJoinResult::ApplyFailed;
    }

    m_applied_revision = m_snapshot_revision;
    WorldRevision expected_revision = m_snapshot_revision;
    for (const WorldDelta& delta : m_buffered) {
        if (delta.epoch != m_epoch ||
            expected_revision == (std::numeric_limits<WorldRevision>::max)() ||
            delta.revision != expected_revision + 1) {
            m_buffered.clear();
            m_state = WorldJoinState::ResnapshotRequired;
            return delta.epoch == m_epoch ? WorldJoinResult::Gap
                                          : WorldJoinResult::WrongEpoch;
        }
        if (!apply_delta(delta)) {
            m_buffered.clear();
            m_state = WorldJoinState::ResnapshotRequired;
            return WorldJoinResult::ApplyFailed;
        }
        expected_revision = delta.revision;
        m_applied_revision = expected_revision;
    }

    m_buffered.clear();
    m_state = WorldJoinState::Ready;
    // m_next_revision was advanced while buffering.  Recompute it from the
    // committed state so this remains correct even if a future caller adds a
    // different transport path that supplies deltas during commit.
    m_next_revision =
        m_applied_revision == (std::numeric_limits<WorldRevision>::max)()
            ? kInvalidWorldRevision
            : m_applied_revision + 1;
    return WorldJoinResult::Ready;
}

bool WorldJoinBarrier::apply_snapshot(const WorldSnapshot& snapshot)
{
    return !m_snapshot_applier || m_snapshot_applier(ByteView{snapshot.payload});
}

bool WorldJoinBarrier::apply_delta(const WorldDelta& delta)
{
    return !m_delta_applier || m_delta_applier(delta);
}

void WorldJoinBarrier::reset() noexcept
{
    m_state = WorldJoinState::Idle;
    m_epoch = kInvalidWorldEpoch;
    m_snapshot_revision = kInvalidWorldRevision;
    m_applied_revision = kInvalidWorldRevision;
    m_next_revision = kInvalidWorldRevision;
    m_buffered.clear();
}

} // namespace kraken::net
