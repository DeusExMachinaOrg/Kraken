#ifndef KRAKEN_NET_WORLD_AUTHORITY_HPP
#define KRAKEN_NET_WORLD_AUTHORITY_HPP

#include <cstdint>

namespace kraken::net::world_authority {

enum class WorldExecutionPhase : std::uint8_t {
    Offline,
    Loading,
    Synchronizing,
    Playing,
    Replay,
    Presentation,
    Teardown,
    Unknown,
};

enum class RuntimeAuthority : std::uint8_t {
    Offline,
    Host,
    Replica,
};

enum class WorldAction : std::uint8_t {
    AiGeneration,
    ScriptWorldMutation,
    QuestAdvance,
    LootGeneration,
    SharedObjectMutation,
    ReplayApply,
    LocalPresentation,
};

struct WorldExecutionContext {
    WorldExecutionPhase phase = WorldExecutionPhase::Unknown;
    RuntimeAuthority authority = RuntimeAuthority::Offline;

    [[nodiscard]] constexpr bool operator==(
        const WorldExecutionContext&) const noexcept = default;
};

namespace detail {

[[nodiscard]] constexpr bool is_known_action(const WorldAction action) noexcept
{
    switch (action) {
    case WorldAction::AiGeneration:
    case WorldAction::ScriptWorldMutation:
    case WorldAction::QuestAdvance:
    case WorldAction::LootGeneration:
    case WorldAction::SharedObjectMutation:
    case WorldAction::ReplayApply:
    case WorldAction::LocalPresentation:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] constexpr bool is_authoritative_action(
    const WorldAction action) noexcept
{
    switch (action) {
    case WorldAction::AiGeneration:
    case WorldAction::ScriptWorldMutation:
    case WorldAction::QuestAdvance:
    case WorldAction::LootGeneration:
    case WorldAction::SharedObjectMutation:
        return true;
    case WorldAction::ReplayApply:
    case WorldAction::LocalPresentation:
    default:
        return false;
    }
}

} // namespace detail

// The policy is intentionally independent of any engine or hook ABI.  Host
// and offline execution preserve native behavior.  Replica lifecycle phases
// retain native loading and teardown, while active/unknown phases only admit
// replay and presentation work.
[[nodiscard]] constexpr bool allows(
    const WorldExecutionPhase phase, const RuntimeAuthority authority,
    const WorldAction action) noexcept
{
    if (!detail::is_known_action(action))
        return false;

    switch (authority) {
    case RuntimeAuthority::Offline:
    case RuntimeAuthority::Host:
        return true;
    case RuntimeAuthority::Replica:
        break;
    default:
        return false;
    }

    switch (phase) {
    case WorldExecutionPhase::Offline:
    case WorldExecutionPhase::Loading:
    case WorldExecutionPhase::Teardown:
        return true;
    case WorldExecutionPhase::Synchronizing:
    case WorldExecutionPhase::Playing:
    case WorldExecutionPhase::Replay:
    case WorldExecutionPhase::Presentation:
    case WorldExecutionPhase::Unknown:
        return !detail::is_authoritative_action(action);
    default:
        return false;
    }
}

[[nodiscard]] WorldExecutionContext current_context() noexcept;

[[nodiscard]] bool allows(WorldAction action) noexcept;

// This scope is deliberately nonmovable: every instance owns one exact
// previous context and must be destroyed in lexical/LIFO order on the game
// thread.  It stores no heap state and changes no engine-global state.
class ScopedWorldExecutionContext final {
public:
    ScopedWorldExecutionContext(WorldExecutionPhase phase,
                                RuntimeAuthority authority) noexcept;
    explicit ScopedWorldExecutionContext(
        WorldExecutionContext context) noexcept;

    ScopedWorldExecutionContext(const ScopedWorldExecutionContext&) = delete;
    ScopedWorldExecutionContext& operator=(
        const ScopedWorldExecutionContext&) = delete;
    ScopedWorldExecutionContext(ScopedWorldExecutionContext&&) = delete;
    ScopedWorldExecutionContext& operator=(
        ScopedWorldExecutionContext&&) = delete;

    ~ScopedWorldExecutionContext() noexcept;

private:
    WorldExecutionContext m_previous{};
};

} // namespace kraken::net::world_authority

#endif // KRAKEN_NET_WORLD_AUTHORITY_HPP
