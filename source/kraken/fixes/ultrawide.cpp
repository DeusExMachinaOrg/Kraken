#define LOGGER "ultrawide"

#include "logger/logger.hpp"
#include "config/config.hpp"
#include "common/routines.hpp"

namespace kraken::fix::ultrawide {
    void Apply()
    {
        const kraken::Config& config = kraken::Config::Instance();
        if (config.ultrawide.value == 0)
            return;

        LOG_INFO("Feature enabled");
        routines::OverrideValue((void*)0x7A6128, (uint32_t) 0x3F800000);
    }
}