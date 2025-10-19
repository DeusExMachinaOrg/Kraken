#define LOGGER "ultrawide"

#include "ext/logger.hpp"
#include "fix/wareuse.hpp"
#include "config.hpp"
#include "routines.hpp"
#include "ext/logger.hpp"

namespace kraken::fix::ultrawide {
    void Apply()
    {
        LOG_INFO("Feature enabled");
        const kraken::Config& config = kraken::Config::Get();
        if (config.ultrawide.value == 0)
            return;
        routines::OverrideValue((void*)0x7A6128, (uint32_t) 0x3F800000);
    }
}