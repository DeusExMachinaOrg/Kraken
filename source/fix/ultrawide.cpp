#define LOGGER "ultrawide"

#include "ext/logger.hpp"
#include "config.hpp"
#include "routines.hpp"
#include "ext/logger.hpp"

namespace kraken::fix::ultrawide {
    void Apply()
    {
        const kraken::Config& config = kraken::Config::Get();
        if (config.ultrawide.value == 0)
            return;

        LOG_INFO("Feature enabled");
        routines::OverrideValue((void*)0x7A6128, (uint32_t) 0x3F800000);
    }
}