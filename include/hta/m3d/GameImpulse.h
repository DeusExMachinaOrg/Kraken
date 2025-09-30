#pragma once
#include "utils.hpp"

#include "Object.h"

namespace m3d
{
	struct IImpulse
	{
		virtual void Dtor() = 0;
	};

	struct KeysSet
	{
		stable_size_set<int> Set;

		bool IsThere(int k)
		{
			FUNC(0x005969B0, bool, __thiscall, _IsThere, KeysSet*, int);
			return _IsThere(this, k);
		}
	};

	struct KeyBindStation
	{
		struct BindKey
		{
			stable_size_vector<m3d::KeysSet> m_keys;
		};

		stable_size_vector<m3d::KeyBindStation::BindKey> m_bindings;
	};

	struct GameImpulse : IImpulse, m3d::Object
	{
		virtual void Dtor() = 0;

		int m_refCount;
		int m_parent;
		bool m_isInited;
		bool m_isBinded;
		bool m_bSuppressEvent;
		stable_size_map<int, m3d::KeyBindStation> m_bindings;
		stable_size_map<int, bool> m_impulseStates;
		stable_size_map<int, bool> m_impulseResetAfterRead;
		m3d::KeysSet CurKeys;
		CStr m_profileFileName;

		bool GetImpulseState(int impId)
		{
			FUNC(0x00597250, bool, __thiscall, _GetImpulseState, GameImpulse*, int);
			return _GetImpulseState(this, impId);
		}
		
		bool GetImpulseStateAndReset(int impId)
		{
			FUNC(0x005946E0, bool, __thiscall, _GetImpulseStateAndReset, GameImpulse*, int);
			return _GetImpulseStateAndReset(this, impId);
		}
	};
}