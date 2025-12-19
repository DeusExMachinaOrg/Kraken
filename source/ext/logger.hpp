#ifndef MTR_LOGGING_HPP
#define MTR_LOGGING_HPP

#include <string>

namespace kraken::logger {
    enum Log {
        eLogDebug,
        eLogInfo,
        eLogWarning,
        eLogError,
        eLogPanic,
    };

    void Init(void);
    void Print(Log level, const char* module, const char* fmt, ...);
    void Flush(void);
}

#ifndef LOGGER
#define LOGGER "Unknown"
#endif

#define LOG_DEBUG(fmt, ...)   kraken::logger::Print(kraken::logger::eLogDebug,   LOGGER, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)    kraken::logger::Print(kraken::logger::eLogInfo,    LOGGER, fmt, ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...) kraken::logger::Print(kraken::logger::eLogWarning, LOGGER, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)   kraken::logger::Print(kraken::logger::eLogError,   LOGGER, fmt, ##__VA_ARGS__)
#define LOG_PANIC(fmt, ...)   kraken::logger::Print(kraken::logger::eLogPanic,   LOGGER, fmt, ##__VA_ARGS__)

#endif