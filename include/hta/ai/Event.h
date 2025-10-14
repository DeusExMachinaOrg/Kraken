#pragma once
#include "Obj.h"
#include "AIParam.h"

namespace ai
{
    enum eGObjPropertySaveStatus
    {
        SAVE_PROP_NORMAL = 0x0,
        SAVE_PROP_ALWAYS = 0x1,
        SAVE_PROP_NEVER = 0x2,
        SAVE_PROP_SPECIAL = 0x3,
    };

    struct Event
    {
        void LoadFromXML(m3d::cmn::XmlFile*, m3d::cmn::XmlNode const*);
        void SaveToXML(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
        CStr Dump() const;
        Event(Event const&);
        Event() = default;

        eGameEvent m_eventId;
        int m_recipientObjId;
        int m_senderObjId;
        float m_timeOut;
        int m_framesToPass;
        float m_timeStamp;
        int m_debugNum;
        m3d::AIParam m_param1;
        m3d::AIParam m_param2;
    };

	ASSERT_SIZE(ai::Event, 0x54);
}
