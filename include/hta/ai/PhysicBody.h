#pragma once
#include <vector>

#include "Obj.hpp"
#include "dMass.h"
#include "GeomTransform.h"
#include "CollisionInfo.h"
#include "hta/Game.h"
#include "hta/m3d/SgNode.hpp"
#include "PhysicObj.h"
#include "ActionType.h"

namespace ai
{

    struct PhysicBodyPrototypeInfo : PrototypeInfo
    {
	    // public:
        virtual void RefreshFromXml(m3d::cmn::XmlFile*, m3d::cmn::XmlNode const*);
        virtual bool LoadFromXML(m3d::cmn::XmlFile*, m3d::cmn::XmlNode const*);
        PhysicBodyPrototypeInfo();

	    // private:
	    int m_engineModelId;
	    CStr m_engineModelName;
	    float m_massValue;
	    stable_size_vector<CollisionInfo> m_collisionInfos;
	    bool m_bCollisionTrimeshAllowed;
    };

	ASSERT_SIZE(ai::PhysicBodyPrototypeInfo, 0x68);

	struct PhysicBody : Obj
	{
		virtual void Dtor() = 0;

		CStr m_modelname;
		dMass mass;
		stable_size_vector<ai::GeomTransform*> m_pGeoms;
		int m_mU;
		m3d::SgNode* Node;
		int m_cfgNum;
		stable_size_vector<ai::CollisionInfo> m_collisionInfos;
		bool m_bCollisionTrimeshAllowed;
		PhysicObj* OwnerPhysicObj;
		bool m_bNeedToRelinkNode;
		int m_animAction;
		int m_effectAction;
		bool m_bAnimationIsStopped;
		int m_loadedAnimTime;

		void SetNodeRelativeRotation(const Quaternion* q)
		{
			FUNC(0x00619D90, void, __thiscall, _SetNodeRelativeRotation, ai::PhysicBody*, const Quaternion*);
			_SetNodeRelativeRotation(this, q);
		}

		void GetNodeRelativeRotation(Quaternion* result)
		{
			FUNC(0x00619C20, void, __thiscall, _GetNodeRelativeRotation, ai::PhysicBody*, const Quaternion*);
			_GetNodeRelativeRotation(this, result);
		}

		void SetEffectActions(stable_size_vector<ActionType>* Actions)
		{
			FUNC(0x0061D180, void, __thiscall, _SetEffectActions, ai::PhysicBody*, stable_size_vector<ActionType>*);
			_SetEffectActions(this, Actions);
		}

		void SetNodeAction(ActionType action, bool forceRestartAction)
		{
			FUNC(0x0061C450, void, __thiscall, _SetEffectActions, ai::PhysicBody*, ActionType, bool);
			_SetEffectActions(this, action, forceRestartAction);
		}

		void SetNodeAnimAction(ActionType action, bool forceRestartAction)
		{
			FUNC(0x0061C5B0, void, __thiscall, _SetNodeAnimAction, ai::PhysicBody*, ActionType, bool);
			_SetNodeAnimAction(this, action, forceRestartAction);
		}

		void SetAnimationStopped(bool bStopped)
		{
			FUNC(0x0061CD30, void, __thiscall, _SetAnimationStopped, ai::PhysicBody*, bool);
			_SetAnimationStopped(this, bStopped);
		}

		static m3d::SgNode* CreateEffectNode(const CStr* modelname, const CVector* pos, const Quaternion* rot, bool bInsertInRemoveIfFree, float scale, bool insertInGraph = 1, int a9 = 0)
		{
			FUNC(0x00617450, m3d::SgNode*, __stdcall, _CreateEffectNode, const CStr*, const CVector*, const Quaternion*, bool, float, bool, int);
			return _CreateEffectNode(modelname, pos, rot, bInsertInRemoveIfFree, scale, insertInGraph, a9);
		}

	};
}