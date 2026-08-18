#include "net/world_replication.hpp"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <vector>

namespace {

using namespace kraken::net;

int failures = 0;

void check(const bool condition, const char* expression, const int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

std::vector<Byte> bytes(std::initializer_list<std::uint8_t> values)
{
    std::vector<Byte> result;
    result.reserve(values.size());
    for (const std::uint8_t value : values)
        result.push_back(static_cast<Byte>(value));
    return result;
}

std::uint8_t first_byte(const WorldDelta& delta)
{
    return delta.payload.empty()
               ? 0
               : std::to_integer<std::uint8_t>(delta.payload.front());
}

void test_host_revisions_retention_and_snapshot_capture()
{
    WorldJournal journal(2);
    CHECK(journal.epoch() != 0);
    CHECK(journal.revision() == 0);

    const WorldDelta first = journal.append(bytes({1}));
    const WorldSnapshot snapshot = journal.capture_snapshot(bytes({10}));
    const WorldDelta second = journal.append(bytes({2}));
    const WorldDelta third = journal.append(bytes({3}));

    CHECK(first.epoch == journal.epoch());
    CHECK(first.revision != 0);
    CHECK(first.revision + 1 == second.revision);
    CHECK(second.revision + 1 == third.revision);
    CHECK(snapshot.epoch == first.epoch);
    CHECK(snapshot.revision == first.revision);
    CHECK(snapshot.payload == bytes({10}));
    CHECK(journal.size() == 2);
    CHECK(journal.deltas()[0].revision == second.revision);
    CHECK(journal.deltas()[1].revision == third.revision);

    std::vector<WorldDelta> replay;
    CHECK(!journal.can_replay_from(journal.epoch(), 0));
    CHECK(journal.can_replay_from(journal.epoch(), first.revision));
    CHECK(journal.deltas_after(journal.epoch(), first.revision, replay));
    CHECK(replay.size() == 2);
    CHECK(replay[0].revision == second.revision);
    CHECK(replay[1].revision == third.revision);
    CHECK(!journal.can_replay_from(journal.epoch() + 1, first.revision));
}

void test_changes_during_snapshot_transfer_commit_in_order()
{
    WorldJournal journal(8);
    const WorldDelta initial = journal.append(bytes({1}));
    const WorldSnapshot snapshot = journal.capture_snapshot(bytes({42}));
    const WorldDelta during_transfer_one = journal.append(bytes({2}));
    const WorldDelta during_transfer_two = journal.append(bytes({3}));

    std::vector<std::uint8_t> applied;
    WorldJoinBarrier barrier(
        8,
        [&applied](ByteView payload) {
            applied.push_back(payload.empty()
                                  ? 0
                                  : std::to_integer<std::uint8_t>(
                                        payload.front()));
            return true;
        },
        [&applied](const WorldDelta& delta) {
            applied.push_back(first_byte(delta));
            return true;
        });

    CHECK(barrier.begin(snapshot) == WorldJoinResult::Started);
    CHECK(!barrier.ready());
    CHECK(barrier.accept_delta(during_transfer_one) ==
          WorldJoinResult::Buffered);
    CHECK(barrier.accept_delta(during_transfer_two) ==
          WorldJoinResult::Buffered);
    CHECK(applied.empty());
    CHECK(barrier.commit(snapshot) == WorldJoinResult::Ready);
    CHECK(barrier.ready());
    CHECK(barrier.state() == WorldJoinState::Ready);
    CHECK(barrier.applied_revision() == during_transfer_two.revision);
    CHECK(applied == std::vector<std::uint8_t>({42, 2, 3}));

    const WorldDelta after_join = journal.append(bytes({4}));
    CHECK(barrier.accept_delta(after_join) == WorldJoinResult::Applied);
    CHECK(barrier.applied_revision() == after_join.revision);
    CHECK(applied.back() == 4);
    CHECK(initial.revision < snapshot.revision + 1);
}

void test_duplicate_out_of_order_gap_and_epoch_rejection()
{
    const WorldSnapshot snapshot{7, 5, bytes({9})};
    const WorldDelta revision_six{7, 6, bytes({6})};
    const WorldDelta revision_seven{7, 7, bytes({7})};
    const WorldDelta revision_eight{7, 8, bytes({8})};
    const WorldDelta wrong_epoch{8, 7, bytes({7})};

    WorldJoinBarrier barrier(4);
    CHECK(barrier.begin(snapshot) == WorldJoinResult::Started);
    CHECK(barrier.accept_delta(revision_eight) == WorldJoinResult::Gap);
    CHECK(barrier.buffered_count() == 0);
    CHECK(barrier.accept_delta(revision_six) == WorldJoinResult::Buffered);
    CHECK(barrier.accept_delta(revision_six) == WorldJoinResult::Duplicate);
    CHECK(barrier.accept_delta(wrong_epoch) == WorldJoinResult::WrongEpoch);
    CHECK(barrier.accept_delta(revision_seven) == WorldJoinResult::Buffered);
    CHECK(barrier.accept_delta(revision_eight) == WorldJoinResult::Buffered);

    std::vector<std::uint8_t> applied;
    CHECK(barrier.commit(
              snapshot,
              [&applied](ByteView) {
                  applied.push_back(0);
                  return true;
              },
              [&applied](const WorldDelta& delta) {
                  applied.push_back(first_byte(delta));
                  return true;
              }) == WorldJoinResult::Ready);
    CHECK(applied == std::vector<std::uint8_t>({0, 6, 7, 8}));
}

void test_epoch_reset_discards_old_journal_and_barrier_transfer()
{
    WorldJournal journal(4);
    const WorldEpoch old_epoch = journal.epoch();
    const WorldDelta old_delta = journal.append(bytes({1}));
    CHECK(journal.reset_epoch());
    CHECK(journal.epoch() != old_epoch);
    CHECK(journal.epoch() != 0);
    CHECK(journal.revision() == 0);
    CHECK(journal.empty());

    const WorldSnapshot snapshot = journal.capture_snapshot(ByteView{});
    const WorldDelta new_delta = journal.append(bytes({2}));
    CHECK(new_delta.epoch == journal.epoch());
    CHECK(new_delta.revision == 1);

    WorldJoinBarrier barrier(4);
    CHECK(barrier.begin(old_delta.epoch, old_delta.revision) ==
          WorldJoinResult::Started);
    CHECK(barrier.accept_delta(new_delta) == WorldJoinResult::WrongEpoch);
    CHECK(barrier.begin(snapshot) == WorldJoinResult::Started);
    CHECK(barrier.accept_delta(old_delta) == WorldJoinResult::WrongEpoch);
    CHECK(barrier.accept_delta(new_delta) == WorldJoinResult::Buffered);
    CHECK(barrier.commit(snapshot) == WorldJoinResult::Ready);
    CHECK(barrier.epoch() == new_delta.epoch);
    CHECK(barrier.applied_revision() == new_delta.revision);
}

void test_overflow_requires_a_fresh_snapshot()
{
    WorldJoinBarrier barrier(1);
    const WorldSnapshot snapshot{11, 0, {}};
    const WorldDelta first{11, 1, bytes({1})};
    const WorldDelta second{11, 2, bytes({2})};

    CHECK(barrier.begin(snapshot) == WorldJoinResult::Started);
    CHECK(barrier.accept_delta(first) == WorldJoinResult::Buffered);
    CHECK(barrier.accept_delta(second) == WorldJoinResult::Overflow);
    CHECK(barrier.resnapshot_required());
    CHECK(!barrier.ready());
    CHECK(barrier.buffered_count() == 0);
    CHECK(barrier.accept_delta(first) == WorldJoinResult::ResnapshotRequired);

    const WorldSnapshot fresh_snapshot{11, 2, bytes({20})};
    CHECK(barrier.begin(fresh_snapshot) == WorldJoinResult::Started);
    CHECK(barrier.commit(fresh_snapshot) == WorldJoinResult::Ready);
    CHECK(barrier.applied_revision() == fresh_snapshot.revision);
}

void test_empty_world_reaches_ready_only_on_commit()
{
    WorldJournal journal;
    const WorldSnapshot snapshot = journal.capture_snapshot(ByteView{});
    CHECK(snapshot.epoch != 0);
    CHECK(snapshot.revision == 0);
    CHECK(snapshot.payload.empty());

    WorldJoinBarrier barrier;
    CHECK(barrier.begin(snapshot) == WorldJoinResult::Started);
    CHECK(barrier.state() == WorldJoinState::Transferring);
    CHECK(!barrier.ready());
    CHECK(barrier.commit(snapshot) == WorldJoinResult::Ready);
    CHECK(barrier.ready());
    CHECK(barrier.applied_revision() == 0);
}

void test_apply_failure_forces_resnapshot()
{
    WorldJoinBarrier barrier(
        2,
        [](ByteView) { return false; },
        [](const WorldDelta&) { return true; });
    const WorldSnapshot snapshot{4, 0, {}};
    CHECK(barrier.begin(snapshot) == WorldJoinResult::Started);
    CHECK(barrier.commit(snapshot) == WorldJoinResult::ApplyFailed);
    CHECK(barrier.state() == WorldJoinState::ResnapshotRequired);
    CHECK(!barrier.ready());
}

} // namespace

int main()
{
    test_host_revisions_retention_and_snapshot_capture();
    test_changes_during_snapshot_transfer_commit_in_order();
    test_duplicate_out_of_order_gap_and_epoch_rejection();
    test_epoch_reset_discards_old_journal_and_barrier_transfer();
    test_overflow_requires_a_fresh_snapshot();
    test_empty_world_reaches_ready_only_on_commit();
    test_apply_failure_forces_resnapshot();

    if (failures != 0) {
        std::cerr << failures << " world replication test(s) failed\n";
        return 1;
    }

    std::cout << "world replication tests passed\n";
    return 0;
}
