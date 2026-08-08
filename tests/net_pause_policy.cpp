#include "net/pause_policy.hpp"

#include <iostream>

int main()
{
    using kraken::net::PauseSignals;
    using kraken::net::should_clear_network_pause;
    const bool passed =
        !should_clear_network_pause({}) &&
        !should_clear_network_pause({false, true, true, false}) &&
        should_clear_network_pause({true, true, false, false}) &&
        should_clear_network_pause({true, false, true, false}) &&
        should_clear_network_pause({true, true, true, false}) &&
        !should_clear_network_pause({true, true, true, true});
    if (!passed) {
        std::cerr << "pause policy tests failed\n";
        return 1;
    }
    std::cout << "pause policy tests passed\n";
    return 0;
}
