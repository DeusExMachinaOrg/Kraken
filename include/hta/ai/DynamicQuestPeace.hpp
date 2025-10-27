#pragma once
#include <hta/ai/PrototypeInfo.h>
#include "Obj.h"
#include "Event.h"

namespace ai {

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
    };

    enum FadingMsgId : int32_t {
        FM_CONVOY_FAILED = 0x0000,
        FM_HUNT_COMPLETED = 0x0001,
        FM_DYNAMIC_QUEST_COMPLETED = 0x0002,
        FM_RELATION_CHANGED = 0x0003,
        FM_TOWN_CANT_ENTER_ENEMY = 0x0004,
        FM_TOWN_CANT_ENTER_LOCKED = 0x0005,
        FM_TOWN_ENTER = 0x0006,
        FM_ITEM_PICKUP = 0x0007,
        FM_ALL_CHESTS_TAKEN = 0x0008,
        FM_TAKE_CHEST_HINT = 0x0009,
        FM_SUNRISE = 0x000a,
        FM_DAY = 0x000b,
        FM_SUNSET = 0x000c,
        FM_NIGHT = 0x000d,
        FM_ITEMS_REMAINED_ON_GROUND = 0x000e,
        FM_PLAYER_ADD_MONEY = 0x000f,
        FM_PLAYER_GIVE_MONEY = 0x0010,
        FM_DYNAMIC_QUEST_FAILED_BECAUSE_HIRER_BECAMES_ENEMY = 0x0011,
    };

    struct DynamicQuest : public Obj {
        enum QuestStatus : int32_t {
            STATUS_NOT_TAKEN = 0x0000,
            STATUS_PROCESSING = 0x0001,
            STATUS_COMPLETE = 0x0002,
            STATUS_FAILED = 0x0003,
            STATUS_FORGOTTEN = 0x0004,
            STATUS_NUM_STATES = 0x0005,
        };
        /* Size=0x110 */
        /* 0x0000: fields for Obj */
        /* 0x00c0 */ int32_t m_reward;
        /* 0x00c8 */ GameTime m_takeGameTime;
        /* 0x00e0 */ FadingMsgId m_fadingMsgIdOnComplete;
        /* 0x00e4 */ bool m_bShowMessageForAddMoney;
        /* 0x00e8 */ QuestStatus m_questStatus;
        /* 0x00ec */ int32_t m_hirerObjId;
        /* 0x00f0 */ int32_t m_targetObjId;
        /* 0x00f4 */ CStr m_hirerName;
        /* 0x0100 */ CStr m_targetName;
        static m3d::Class m_classDynamicQuest;
        static stable_size_map<CStr, int> m_propertiesMap;
        static stable_size_map<int, eGObjPropertySaveStatus> m_propertiesSaveStatesMap;
    };

    struct DynamicQuestManager {
        enum QuestType : int32_t {
            TYPE_DESTROY = 0x0000,
            TYPE_REACH = 0x0001,
            TYPE_CONVOY = 0x0002,
            TYPE_PEACE = 0x0003,
            TYPE_HUNT = 0x0004,
            TYPE_NUM_TYPES = 0x0005,
        };
        /* Size=0x1 */

        DynamicQuestManager();

        static DynamicQuest* CreateQuest(QuestType, int32_t, int32_t);
        static DynamicQuest* CreateQuest(QuestType, const CStr&, const CStr&);
        static void ConsiderPlayerKill(int32_t);
    };

    struct DynamicQuestPrototypeInfo : public PrototypeInfo {
        /* Size=0x44 */
        /* 0x0000: fields for PrototypeInfo */
        /* 0x0040 */ int32_t m_minReward;

        DynamicQuestPrototypeInfo();
        virtual bool LoadFromXML(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual ~DynamicQuestPrototypeInfo();
    };

    struct DynamicQuestPeacePrototypeInfo : public DynamicQuestPrototypeInfo {
        /* Size=0x48 */
        /* 0x0000: fields for DynamicQuestPrototypeInfo */
        /* 0x0044 */ float m_playerMoneyPart;

        DynamicQuestPeacePrototypeInfo();
        virtual bool LoadFromXML(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual DynamicQuest* CreateTargetObject() const;
        virtual ~DynamicQuestPeacePrototypeInfo();
    };

    struct DynamicQuestPeace : public DynamicQuest {
        /* Size=0x110 */
        /* 0x0000: fields for DynamicQuest */
        static m3d::Class m_classDynamicQuestPeace;

        virtual ~DynamicQuestPeace();
        DynamicQuestPeace(const DynamicQuestPeacePrototypeInfo&);
        DynamicQuestPeace(const DynamicQuestPeace&);
        virtual m3d::Object* Clone();
        virtual m3d::Class* GetClass() const;
        virtual const DynamicQuestPeacePrototypeInfo* GetPrototypeInfo() const;
        virtual int32_t OnEvent(const Event&);
        virtual DynamicQuestManager::QuestType GetQuestType() const;
        virtual void PassToAnotherMap();
        virtual void _OnCreate();
        virtual void _OnTake();
        virtual int32_t _CalcReward();
        virtual void _OnHirerBecamesEnemyWithPlayer();

        static m3d::Object* CreateObject();
        static m3d::Class* GetBaseClass();
    };
}


// BRB