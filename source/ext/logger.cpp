#include "ext/logger.hpp"

#include <fstream>
#include <chrono>

#include <windows.h>

namespace kraken::logger {
    static struct {
        const char*   path          { "./kraken.log" };
        char          message[2048] { 0              };
        char          stamp[32]     { 0              };
        char          time[32]      { 0              };
        bool          is_stream     { false          };
        bool          is_file       { false          };
        bool          is_ready      { false          };
        std::ofstream handle        {                };
    } self;

    bool _InitFile() {
        self.handle.open(self.path, std::ofstream::out | std::ofstream::trunc);
        if (!self.handle.is_open())
            return false;
        return true;
    };

    bool _InitStream() {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
            DWORD error = GetLastError();
            if (error == ERROR_ACCESS_DENIED) {
            }
            else if (error == ERROR_INVALID_HANDLE) {
                if (!AllocConsole()) {
                    return false;
                }
            }
            else {
                return false;
            }
        }

        FILE* dummy;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONIN$", "r", stdin);
        freopen_s(&dummy, "CONOUT$", "w", stderr);

        return true;
    };

    const char* _LevelName(Log level) {
        switch (level) {
            case eLogDebug:   return "DEBUG";
            case eLogInfo:    return "INFO";
            case eLogWarning: return "WARNING";
            case eLogError:   return "ERROR";
            case eLogPanic:   return "PANIC";
            default:          return "UNKNOWN";
        }
    };

    void Init(void) {
        if (self.is_ready)
            return;

        self.is_file   = _InitFile();
        self.is_stream = _InitStream();
        self.is_ready  = self.is_file || self.is_stream;
    };

    void Print(Log level, const char* alias, const char* fmt, ...) {
        if (!self.is_ready)
            return;

        va_list args;
        va_start(args, fmt);
        int32_t size = std::vsnprintf(self.message, sizeof(self.message), fmt, args);
        va_end(args);

        if (size <= 0)
            return;

        std::tm tm_buf;
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        localtime_s(&tm_buf, &time_t);

        std::strftime(self.time, sizeof(self.time), "%Y-%m-%d %H:%M:%S", &tm_buf);
        std::snprintf(self.stamp, sizeof(self.stamp), "%s.%03d", self.time, static_cast<int>(ms.count()));

        const char* lname = _LevelName(level);

        if (self.is_file) {
            self.handle << lname << " " << self.stamp << " " << alias << " - " << self.message << std::endl;
            self.handle.flush();
        }

        if (self.is_stream) {
            std::printf("%-8s %s %s - %s\n", lname, self.stamp, alias, self.message);
        }
    };

    void Flush(void) {
        if (self.is_file)
            self.handle.flush();
    };
}