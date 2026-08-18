#include "net/world_authority.hpp"
#include "net/runtime.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <iostream>
#include <utility>

namespace {

using namespace kraken::net::world_authority;

constexpr std::array<WorldExecutionPhase, 8> kPhases{
    WorldExecutionPhase::Offline,
    WorldExecutionPhase::Loading,
    WorldExecutionPhase::Synchronizing,
    WorldExecutionPhase::Playing,
    WorldExecutionPhase::Replay,
    WorldExecutionPhase::Presentation,
    WorldExecutionPhase::Teardown,
    WorldExecutionPhase::Unknown,
};

constexpr std::array<RuntimeAuthority, 3> kAuthorities{
    RuntimeAuthority::Offline,
    RuntimeAuthority::Host,
    RuntimeAuthority::Replica,
};

constexpr std::array<WorldAction, 7> kActions{
    WorldAction::AiGeneration,
    WorldAction::ScriptWorldMutation,
    WorldAction::QuestAdvance,
    WorldAction::LootGeneration,
    WorldAction::SharedObjectMutation,
    WorldAction::ReplayApply,
    WorldAction::LocalPresentation,
};

// [phase][authority][action].  The table is intentionally explicit so every
// policy cell remains visible when a future native hook adds a call site.
constexpr bool kExpected[8][3][7]{
    // Offline phase: offline/host/replica preserve native lifecycle behavior.
    {{true, true, true, true, true, true, true},
     {true, true, true, true, true, true, true},
     {true, true, true, true, true, true, true}},
    // Loading: replica map/static/script initialization is not filtered.
    {{true, true, true, true, true, true, true},
     {true, true, true, true, true, true, true},
     {true, true, true, true, true, true, true}},
    // Synchronizing: only replay and local presentation may execute.
    {{true, true, true, true, true, true, true},
     {true, true, true, true, true, true, true},
     {false, false, false, false, false, true, true}},
    // Playing: replicas cannot originate authoritative world actions.
    {{true, true, true, true, true, true, true},
     {true, true, true, true, true, true, true},
     {false, false, false, false, false, true, true}},
    // Replay: replay application and presentation remain local operations.
    {{true, true, true, true, true, true, true},
     {true, true, true, true, true, true, true},
     {false, false, false, false, false, true, true}},
    // Presentation: presentation/replay work remains local to the replica.
    {{true, true, true, true, true, true, true},
     {true, true, true, true, true, true, true},
     {false, false, false, false, false, true, true}},
    // Teardown: native destruction and route completion are unfiltered.
    {{true, true, true, true, true, true, true},
     {true, true, true, true, true, true, true},
     {true, true, true, true, true, true, true}},
    // Unknown: fail closed for authoritative work, retain local operations.
    {{true, true, true, true, true, true, true},
     {true, true, true, true, true, true, true},
     {false, false, false, false, false, true, true}},
};

void test_exhaustive_policy_matrix()
{
    for (std::size_t phase = 0; phase < kPhases.size(); ++phase) {
        for (std::size_t authority = 0; authority < kAuthorities.size();
             ++authority) {
            for (std::size_t action = 0; action < kActions.size(); ++action) {
                assert(allows(kPhases[phase], kAuthorities[authority],
                              kActions[action]) ==
                       kExpected[phase][authority][action]);
            }
        }
    }
}

void test_nested_scopes_restore_exact_context()
{
    assert((current_context() == WorldExecutionContext{
                                    WorldExecutionPhase::Unknown,
                                    RuntimeAuthority::Offline}));
    {
        ScopedWorldExecutionContext outer(WorldExecutionPhase::Playing,
                                          RuntimeAuthority::Replica);
        assert((current_context() == WorldExecutionContext{
                                        WorldExecutionPhase::Playing,
                                        RuntimeAuthority::Replica}));
        assert(!allows(WorldAction::AiGeneration));
        assert(allows(WorldAction::ReplayApply));

        {
            ScopedWorldExecutionContext inner(WorldExecutionPhase::Replay,
                                              RuntimeAuthority::Replica);
            assert((current_context() == WorldExecutionContext{
                                            WorldExecutionPhase::Replay,
                                            RuntimeAuthority::Replica}));
            assert(!allows(WorldAction::QuestAdvance));
            assert(allows(WorldAction::LocalPresentation));
        }

        assert((current_context() == WorldExecutionContext{
                                        WorldExecutionPhase::Playing,
                                        RuntimeAuthority::Replica}));
        assert(!allows(WorldAction::SharedObjectMutation));
    }
    assert((current_context() == WorldExecutionContext{
                                    WorldExecutionPhase::Unknown,
                                    RuntimeAuthority::Offline}));
}

void test_replica_replay_and_presentation()
{
    for (const WorldExecutionPhase phase : {WorldExecutionPhase::Replay,
                                            WorldExecutionPhase::Presentation}) {
        ScopedWorldExecutionContext scope(phase, RuntimeAuthority::Replica);
        for (std::size_t action = 0; action < 5; ++action)
            assert(!allows(kActions[action]));
        assert(allows(WorldAction::ReplayApply));
        assert(allows(WorldAction::LocalPresentation));
    }
}

void test_replica_unknown_fails_closed()
{
    ScopedWorldExecutionContext scope(WorldExecutionPhase::Unknown,
                                      RuntimeAuthority::Replica);
    for (std::size_t action = 0; action < 5; ++action)
        assert(!allows(kActions[action]));
    assert(allows(WorldAction::ReplayApply));
    assert(allows(WorldAction::LocalPresentation));
}

void test_loading_and_teardown_are_unfiltered()
{
    for (const WorldExecutionPhase phase : {WorldExecutionPhase::Loading,
                                            WorldExecutionPhase::Teardown}) {
        ScopedWorldExecutionContext scope(phase, RuntimeAuthority::Replica);
        for (const WorldAction action : kActions)
            assert(allows(action));
    }
}

void test_policy_apis_are_noexcept()
{
    static_assert(noexcept(allows(WorldExecutionPhase::Playing,
                                  RuntimeAuthority::Replica,
                                  WorldAction::AiGeneration)));
    static_assert(noexcept(allows(WorldAction::ReplayApply)));
    static_assert(noexcept(current_context()));
    static_assert(noexcept(ScopedWorldExecutionContext(
        WorldExecutionPhase::Loading, RuntimeAuthority::Replica)));
    static_assert(noexcept(std::declval<ScopedWorldExecutionContext&>()
                          .~ScopedWorldExecutionContext()));
}

void test_runtime_context_comes_from_session_lifecycle()
{
    using kraken::net::MatchState;
    using kraken::net::runtime::detail::derive_world_execution_context;
    using kraken::net::world_authority::RuntimeAuthority;
    using kraken::net::world_authority::WorldExecutionContext;
    using kraken::net::world_authority::WorldExecutionPhase;

    assert((derive_world_execution_context(
        false, false, MatchState::Playing, MatchState::Playing) ==
        WorldExecutionContext{WorldExecutionPhase::Offline,
                              RuntimeAuthority::Offline}));
    assert((derive_world_execution_context(
        true, true, MatchState::Synchronizing, MatchState::Offline) ==
        WorldExecutionContext{WorldExecutionPhase::Synchronizing,
                              RuntimeAuthority::Host}));
    assert((derive_world_execution_context(
        true, false, MatchState::Offline, MatchState::Loading) ==
        WorldExecutionContext{WorldExecutionPhase::Loading,
                              RuntimeAuthority::Replica}));
    assert((derive_world_execution_context(
        true, false, MatchState::Offline, MatchState::Forming) ==
        WorldExecutionContext{WorldExecutionPhase::Loading,
                              RuntimeAuthority::Replica}));

    const WorldExecutionContext playing = derive_world_execution_context(
        true, false, MatchState::Offline, MatchState::Playing);
    assert(playing.phase == WorldExecutionPhase::Playing);
    assert(playing.authority == RuntimeAuthority::Replica);
    assert(!allows(playing.phase, playing.authority, WorldAction::AiGeneration));
    assert(!allows(playing.phase, playing.authority,
                   WorldAction::ScriptWorldMutation));
    assert(!allows(playing.phase, playing.authority, WorldAction::QuestAdvance));
    assert(allows(playing.phase, playing.authority, WorldAction::ReplayApply));
    assert(allows(playing.phase, playing.authority,
                  WorldAction::LocalPresentation));

    assert((derive_world_execution_context(
        true, false, MatchState::Offline, MatchState::Playing, true, false) ==
        WorldExecutionContext{WorldExecutionPhase::Replay,
                              RuntimeAuthority::Replica}));
    assert((derive_world_execution_context(
        true, false, MatchState::Offline, MatchState::Playing, false, true) ==
        WorldExecutionContext{WorldExecutionPhase::Presentation,
                              RuntimeAuthority::Replica}));
    assert((derive_world_execution_context(
        true, false, MatchState::Offline, MatchState::Leaving) ==
        WorldExecutionContext{WorldExecutionPhase::Teardown,
                              RuntimeAuthority::Replica}));
}

} // namespace

int main()
{
    test_exhaustive_policy_matrix();
    test_nested_scopes_restore_exact_context();
    test_replica_replay_and_presentation();
    test_replica_unknown_fails_closed();
    test_loading_and_teardown_are_unfiltered();
    test_policy_apis_are_noexcept();
    test_runtime_context_comes_from_session_lifecycle();
    std::cout << "world authority tests passed\n";
    return 0;
}
