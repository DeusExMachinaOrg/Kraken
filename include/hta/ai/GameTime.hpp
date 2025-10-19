#pragma once

#include <stdint.h>
#include "hta/m3d/AIParam.hpp"

namespace m3d::cmn {
    struct XmlFile;
    struct XmlNode;
}

namespace ai {
    struct ObjContainer;

    struct GameTime {
        /* Size=0x18 */
        /* 0x0008 */ int64_t m_milliSeconds;
        /* 0x0010 */ int64_t m_milliSeconds0;

    static const int64_t SecondBase;
    static const int64_t MinuteBase;
    static const int64_t HourBase;
    static const int64_t DayBase;
    static const int64_t MonthBase;
    static const int64_t YearBase;
    
    GameTime(const GameTime&);
    GameTime(int64_t);
    GameTime(int32_t, int32_t, int32_t, int32_t, int32_t);
    GameTime();
    void setInt64(int64_t);
    void setExpanded(int32_t, int32_t, int32_t, int32_t, int32_t);
    int64_t asInt64() const;
    m3d::AIParam asAIParam24Hour() const;
    m3d::AIParam asAIParam() const;
    void operator+=(float);
    float Diff() const;
    float GameDiff(const ObjContainer*) const;
    virtual void LoadFromXML(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
    virtual void SaveToXML(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
    };
};