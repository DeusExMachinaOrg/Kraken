#pragma once
#include <utils.hpp>

#include "hta/m3d/Object.h"
#include "Modifier.h"

namespace ai
{
	struct Obj : m3d::Object
	{
		virtual void Dtor() = 0;

		void Remove()
		{
			FUNC(0x00692880, void, __thiscall, _Remove, Obj*);
			return _Remove(this);
		}

		struct EventRecipientInfo
		{
			int m_eventId;
			stable_size_vector<int> m_objIds;
		};

		int objId;
		int updatingObjId;
		bool isUpdating;
		bool mustBeUpdating;
		int flags;
		int m_actionID;
		int m_prevActionID;
		float m_timeOut;
		bool m_bNeedPostLoad;
		bool m_bMustCreateVisualPart;
		bool m_bPassedToAnotherMap;
		int m_belong;
		int m_parentId;
		int m_parentRepository;
		int m_LastDamageSource;
		bool m_bIsAlreadySaved;
		int m_hierarchyType;
		int PrototypeId;
		stable_size_map<int, ai::Obj*> m_allChildren;
		stable_size_vector<int> m_appliedPrefixIds;
		stable_size_vector<int> m_appliedSuffixIds;
		bool m_bAffixesWasApplied;
		stable_size_vector<ai::Modifier> m_modifiers;
		stable_size_vector<ai::Obj::EventRecipientInfo> m_eventRecipients;
	};

	ASSERT_SIZE(ai::Obj, 0xC0);
}