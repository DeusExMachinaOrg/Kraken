#pragma once

#include <stdint.h>
#include <string>

namespace m3d {
    struct Profiler {
        /* Size=0x58 */
        /* 0x0008 */ std::basic_string<char> m_name;
        /* 0x0024 */ uint32_t m_curFrame;
        /* 0x0028 */ uint32_t m_numFramesToRecalculate;
        /* 0x002c */ float m_performanceCounterFrequency;
        /* 0x0030 */ int64_t m_totalClocks;
        /* 0x0038 */ int64_t m_totalClocksPerFrame;
        /* 0x0040 */ int64_t m_lastClocks;
        /* 0x0048 */ int64_t m_totalClocksForRecalcFrames;
        /* 0x0050 */ int64_t m_averageClocks;

        Profiler(const Profiler&);
        Profiler(const char*);
        Profiler();
        virtual ~Profiler();
        void SetName(const char*);
        const char* GetName() const;
        void StartFrame();
        void EndFrame();
        void StartCountdown();
        void EndCountdown();
        void Reset();
        uint32_t GetTotalTime() const;
        uint32_t GetLastFrameTime() const;
        void SetAverageVal(uint32_t);
        double GetAverageTime() const;
        void Init();
};
};