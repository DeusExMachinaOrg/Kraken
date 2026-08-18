#include "net/world_authority.hpp"

namespace kraken::net::world_authority {
namespace {

thread_local WorldExecutionContext g_current_context{};

} // namespace

WorldExecutionContext current_context() noexcept
{
    return g_current_context;
}

bool allows(const WorldAction action) noexcept
{
    const WorldExecutionContext context = current_context();
    return allows(context.phase, context.authority, action);
}

ScopedWorldExecutionContext::ScopedWorldExecutionContext(
    const WorldExecutionPhase phase, const RuntimeAuthority authority) noexcept
    : ScopedWorldExecutionContext(WorldExecutionContext{phase, authority})
{
}

ScopedWorldExecutionContext::ScopedWorldExecutionContext(
    const WorldExecutionContext context) noexcept
    : m_previous(g_current_context)
{
    g_current_context = context;
}

ScopedWorldExecutionContext::~ScopedWorldExecutionContext() noexcept
{
    g_current_context = m_previous;
}

} // namespace kraken::net::world_authority
