#pragma once
#include "hta/Game.h"
#include "hta/ai/Vehicle.h"
#include "ai/FuncPtr.hpp"

namespace ai
{

	template<typename T>
    struct NumericBoundedBelow : public Component<T> {
        /* Size=0x78 (120 bytes) */
        /* 0x0000: fields for Component<T> (0x10 = 16 bytes) */
        
        // Modifier callbacks (before applying modifiers)
        /* 0x0010 */ FuncPtrTwoArgsRef<Modifier, T, bool> m_BeforeValueApplyModifier;
        /* 0x0018 */ FuncPtrTwoArgsRef<Modifier, T, bool> m_BeforeMinValueApplyModifier;
        
        // Change validation callbacks (before value changes)
        /* 0x0020 */ FuncPtrOneArgRef<T, bool> m_BeforeValueChange;
        /* 0x0028 */ FuncPtrOneArgRef<T, bool> m_BeforeMinValueChange;
        
        // Notification callbacks (after value changes)
        /* 0x0030 */ FuncPtrOneArg<T, void> m_AfterValueChange;
        /* 0x0038 */ FuncPtrOneArg<T, void> m_AfterMinValueChange;
        
        // The actual numeric values with their own callbacks
        /* 0x0040 */ Numeric<T> m_value;      // 0x1c bytes (28 bytes)
        /* 0x005c */ Numeric<T> m_minValue;   // 0x1c bytes (28 bytes)

        // Constructor
        NumericBoundedBelow(T, T);
        
        // Accessors
        const Numeric<T>& minValue() const;
        Numeric<T>& minValue();
        const Numeric<T>& value() const;
        Numeric<T>& value();
        
        // Operations
        void assign(const NumericBoundedBelow<T>&);
        void setToMin();
        bool bIsMin() const;
        
        // Internal methods
        void _AssignUnsafe(const NumericBoundedBelow<T>&);
        void _AfterSomeChange();
        
        // Callback handlers - After change notifications
        void _OnAfterValueChange(T);
        void _OnAfterMinValueChange(T);
        
        // Callback handlers - Before change validation
        bool _OnBeforeValueChange(T&);
        bool _OnBeforeMinValueChange(T&);
        
        // Callback handlers - Before modifier application
        bool _OnBeforeValueApplyModifier(const Modifier&, T&);
        bool _OnBeforeMinValueApplyModifier(const Modifier&, T&);
        
        ~NumericBoundedBelow();
    };

	struct Player
	{
		inline static Player*& Instance = *(Player**)0x00A135E4;

		struct InfoCone;
		struct RadioManager;

        enum PlayerFightState : int32_t {
		    FIGHT_CLEAR = 0x0000,
		    FIGHT_ALARM = 0x0001,
		    FIGHT_BATTLE = 0x0002,
		    FIGHT_BATTLE_JUST_FINISHED = 0x0003,
		    NUM_FIGHT_STATES = 0x0004,
        };
        /* Size=0x264 */
        /* 0x0000: fields for Obj */
        /* 0x00c0 */ NumericBoundedBelow<int> m_money;
        /* 0x0138 */ int32_t m_vehicleObjId;
        /* 0x013c */ RadioManager* m_radioManager;
        /* 0x0140 */ stable_size_vector<CStr> m_questItemPrototypeNames;
        /* 0x0150 */ int32_t m_infoObjId;
        /* 0x0154 */ InfoCone* m_infoCone;
        /* 0x0158 */ float m_timeInfoObjTimeout;
        /* 0x015c */ PlayerFightState m_playerFightState;
        /* 0x0160 */ PlayerFightState m_prevPlayerFightState;
        /* 0x0164 */ NumericInRangeRegenerating<float> m_timeOfNoBattle;
        /* 0x023c */ CStr m_lastSaveDir;
        /* 0x0248 */ CStr m_modelName;
        /* 0x0254 */ uint32_t m_skinNumber;
        /* 0x0258 */ uint32_t m_cfgNumber;
        /* 0x025c */ bool m_huntQuestIsTaken;
        /* 0x0260 */ int32_t m_curNumForVehicleWithoutName;
        static m3d::Class m_classPlayer;
        static stable_size_map<CStr, int> m_propertiesMap;
        static stable_size_map<int, eGObjPropertySaveStatus> m_propertiesSaveStatesMap;

		ai::Vehicle* GetVehicle()
		{
			FUNC(0x00651190, ai::Vehicle*, __thiscall, _GetVehicle, ai::Player*);
			return _GetVehicle(this);
		}

		int32_t GetMoney() const
		{
			FUNC(0x0064FA80, int32_t, __thiscall, _GetMoney, const ai::Player*);
			return _GetMoney(this);
		}

		int32_t GetSchwarz() const
		{
			FUNC(0x00651380, int32_t, __thiscall, _GetSchwarz, const ai::Player*);
			return _GetSchwarz(this);
		}
	};
}