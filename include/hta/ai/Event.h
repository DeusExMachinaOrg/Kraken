#pragma once
#include "hta/m3d/AIParam.h"

namespace ai {
    enum eGameEvent : __int32
    {
        GE_UNKNOWN                      = 0x0,
        GE_GAME_START                   = 0x1,
        GE_SUBSCRIBE                    = 0x2,
        GE_UNSUBSCRIBE                  = 0x3,
        GE_OBJECT_ENTERS_LOCATION       = 0x4,
        GE_OBJECT_LEAVES_LOCATION       = 0x5,
        GE_OBJECT_IN_LOCATION           = 0x6,
        GE_OBJECT_ACTIVATED             = 0x7,
        GE_OBJECT_DEACTIVATED           = 0x8,
        GE_OBJECT_DIE                   = 0x9,
        GE_OBJECT_DIE_SENSE             = 0xA,
        GE_END_TTL                      = 0xB,
        GE_END_ANIMATION                = 0xC,
        GE_TIME_PERIOD                  = 0xD,
        GE_FRAMES_PASSED                = 0xE,
        GE_FREE                         = 0xF,
        GE_ENEMY_LOST                   = 0x10,
        GE_ENEMY_DESTROYED              = 0x11,
        GE_TUTORIAL_VEHICLE             = 0x12,
        GE_TUTORIAL_INVENTORY           = 0x13,
        GE_TUTORIAL_TOWN                = 0x14,
        GE_TUTORIAL_BAR                 = 0x15,
        GE_TUTORIAL_WORKSHOP            = 0x16,
        GE_TUTORIAL_SHOP                = 0x17,
        GE_TUTORIAL_QUESTLOG            = 0x18,
        GE_TUTORIAL_MAP                 = 0x19,
        GE_TUTORIAL_STATS               = 0x1A,
        GE_TUTORIAL_HISTORY             = 0x1B,
        GE_TUTORIAL_BOOKS               = 0x1C,
        GE_TUTORIAL_ENCYCLOPAEDIA       = 0x1D,
        GE_TUTORIAL_RELATIONS           = 0x1E,
        GE_TUTORIAL_JOURNAL             = 0x1F,
        GE_TUTORIAL_TAB_GOODS           = 0x20,
        GE_TUTORIAL_TAB_WEAPON          = 0x21,
        GE_TUTORIAL_CABINS              = 0x22,
        GE_TUTORIAL_CABIN_SELECT        = 0x23,
        GE_TUTORIAL_BASKETS             = 0x24,
        GE_TUTORIAL_BASKET_SELECT       = 0x25,
        GE_TUTORIAL_NEW_VEHICLE         = 0x26,
        GE_TUTORIAL_NEW_VEHICLE_SELECT  = 0x27,
        GE_TUTORIAL_SKIN                = 0x28,
        GE_TUTORIAL_REFUEL              = 0x29,
        GE_TUTORIAL_REPAIR              = 0x2A,
        GE_TUTORIAL_RECHARGE            = 0x2B,
        GE_PART_BROKEN                  = 0x2C,
        GE_RELATION_CHANGED             = 0x2D,
        GE_NOTICE_SOMEONE               = 0x2E,
        GE_UNDER_ATTACK                 = 0x2F,
        GE_TEAM_NEEDS_REINFORCEMENT     = 0x30,
        GE_LOST_GUARDS_NEED_DIRECTION   = 0x31,
        GE_NOTICE_ENEMY                 = 0x32,
        GE_TARGET_REACHED               = 0x33,
        GE_TARGET_UNREACHED             = 0x34,
        GE_OBJECT_ENTERS_OBJECT         = 0x35,
        GE_OBJECT_LEAVES_OBJECT         = 0x36,
        GE_TALK_WITH_OBJECT             = 0x37,
        GE_SKIP_CINEMATIC               = 0x38,
        GE_END_CINEMATIC                = 0x39,
        GE_START_CINEMATIC_MSG          = 0x3A,
        GE_START_CINEMATIC_FLY          = 0x3B,
        GE_IN_CINEMATIC                 = 0x3C,
        GE_CINEMATIC_ENTER_FADE_IN      = 0x3D,
        GE_CUSTOM_GUN_POINTED           = 0x3E,
        GE_CUSTOM_GUN_DISPOINTED        = 0x3F,
        GE_PLAYER_VEHICLE_HORN          = 0x40,
        GE_PLAYER_VEHICLE_CHANGED       = 0x41,
        GE_VEHICLE_WITHOUT_HEALTH       = 0x42,
        GE_BOSS_CRITICAL_LOADS_EXPLODED = 0x43,
        GE_BOSS_ARM_ACTION_FINISHED     = 0x44,
        GE_BOSS04_STATION_DESTROYED     = 0x45,
        GE_LEAVE_TOWN                   = 0x46,
        GE_TOWN_CONDITIONAL_CLOSING     = 0x47,
        GE_DYNAMIC_QUEST_TAKEN          = 0x48,
        GE_DYNAMIC_QUEST_COMPLETE       = 0x49,
        GE_DYNAMIC_QUEST_FORGOTTEN      = 0x4A,
        GE_DYNAMIC_QUEST_FAILED         = 0x4B,
        GE_NUM_EVENTS                   = 0x4C,
    };

    struct Event {
        ai::eGameEvent m_eventId;
        int m_recipientObjId;
        int m_senderObjId;
        float m_timeOut;
        int m_framesToPass;
        float m_timeStamp;
        int m_debugNum;
        m3d::AIParam m_param1;
        m3d::AIParam m_param2;
    };
}