#pragma once

#include <cstdint>

namespace kraken::net {

// A prototype that successfully constructs a non-vehicle cannot become a
// vehicle by repeating the same creation every frame. Keep that failure
// terminal for the owning peer/entity generation; reset only when ownership
// or generation state is rebuilt.
class SpawnAttemptState {
public:
    [[nodiscard]] bool can_attempt(const std::uint32_t current_tick) const noexcept
    {
        return !terminal_failure_ &&
            static_cast<std::int32_t>(current_tick - retry_after_tick_) >= 0;
    }

    void defer(const std::uint32_t current_tick,
               const std::uint32_t delay_ticks) noexcept
    {
        retry_after_tick_ = current_tick + (delay_ticks == 0 ? 1 : delay_ticks);
    }

    void reject_permanently() noexcept
    {
        terminal_failure_ = true;
    }

    void reset() noexcept
    {
        terminal_failure_ = false;
        retry_after_tick_ = 0;
    }

private:
    bool terminal_failure_ = false;
    std::uint32_t retry_after_tick_ = 0;
};

} // namespace kraken::net
