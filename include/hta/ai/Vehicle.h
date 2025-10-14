#pragma once
#include <deque>
#include "ComplexPhysicObj.h"
#include "IzvratRepository.h"
#include "ActionType.h"
#include "VehiclePrototypeInfo.h"
#include "Basket.h"
#include "Wheel.h"
#include "Cabin.h"
#include "Chassis.h"
#include "Gadget.h"
#include "Path.h"
#include "ref_ptr.h"
#include "scoped_ptr.h"
#include "Geom.h"
#include "hta/m3d/Object.h"
#include "SgNode.h"

struct dxGeom;
struct dxJoint;

namespace m3d
{
	namespace cmn
	{
		struct XmlFile;
		struct XmlNode;
	}

	struct SgSoundSourceNode;
}

namespace ai
{
	struct VehicleUpdater;

	struct SphereForIntersection;

	struct Box : Geom { };

	struct Obstacle
	{
  		int32_t m_refCount;
  		bool m_bIsEnabled;
  		void* m_intersectionSphere;
  		void* m_intersectionBox;
  		int32_t m_ownerPhysicObjId;
  		m3d::SgNode* m_ownerSgNode;
	};

	struct AIPassageState
	{
		int m_StateNum;
		std::vector<m3d::AIParam,std::allocator<m3d::AIParam> > m_ParamList;
		void LoadFromXML(m3d::cmn::XmlFile* xmlFile, const m3d::cmn::XmlNode* OwnNode);
		void SaveToXML(m3d::cmn::XmlFile* xmlFile, m3d::cmn::XmlNode* OwnNode) const;
	};

	struct DecisionMatrix;

	struct AIMessage
	{
		int m_Num;
		int m_RemoveAfterFinishing;
		std::vector<m3d::AIParam,std::allocator<m3d::AIParam> > m_ParamList;
		void LoadFromXML(m3d::cmn::XmlFile* xmlFile, const m3d::cmn::XmlNode* OwnNode);
		void SaveToXML(m3d::cmn::XmlFile* xmlFile, m3d::cmn::XmlNode* OwnNode) const;
		AIMessage(const ai::AIMessage& __that);
		AIMessage(int Num, const m3d::AIParam& Param1, const m3d::AIParam& Param2, const m3d::AIParam& Param3);
		AIMessage();
		bool operator==(const ai::AIMessage& message);
		bool operator!=(const ai::AIMessage&);
	};

	struct AI
	{
		AI(const ai::AI&);
		AI();
		void AIInit();
		const CStr& GetCurState2Name();
		const CStr& GetCurState1Name();
		int GetCurState2Num();
		int GetCurState1Num();
		m3d::AIParam GetCmdParam(unsigned int paramNum);
		m3d::AIParam GetState1Param(unsigned int paramNum);
		m3d::AIParam GetState2Param(unsigned int paramNum);
		m3d::AIParam GetMessage1Param(unsigned int paramNum);
		m3d::AIParam GetMessage2Param(unsigned int paramNum);
		void SetDecisionMatrix(int MatrixNum);
		ai::DecisionMatrix* GetDecisionMatrixPtr();
		void AIUpdate(ai::Obj* pObj);
		void SetState2Param(int ParamNum, const m3d::AIParam& Param);
		void SetState1Param(int ParamNum, const m3d::AIParam& Param);
		void PutCommand(int Num, const m3d::AIParam& Param1, const m3d::AIParam& Param2, const m3d::AIParam& Param3);
		void PutCommand(const ai::AIMessage& command);
		void InsCommand(int Num, const m3d::AIParam& Param1, const m3d::AIParam& Param2, const m3d::AIParam& Param3);
		void SetCommand(int Num, const m3d::AIParam& Param1, const m3d::AIParam& Param2, const m3d::AIParam& Param3);
		void CommandStackOpen();
		void CommandStackClose();
		void PutMessage2(const ai::AIMessage& Message);
		void PutMessage1(const ai::AIMessage& Message);
		void LoadAIFromXML(m3d::cmn::XmlFile* xmlFile, m3d::cmn::XmlNode* OwnNode);
		void SaveAIToXML(m3d::cmn::XmlFile* xmlFile, m3d::cmn::XmlNode* OwnNode) const;
		void Dump();
		CStr ToStr();
		
		private:
			int _CurrentState2Num();
			int _CurrentState1Num();
			int _CurrentMessage2CommandNum();
			int _CurrentMessage1CommandNum();
			std::vector<ai::AIPassageState> m_StateStack2;
			std::vector<ai::AIPassageState> m_StateStack1;
			ai::DecisionMatrix* m_pDM;
			bool m_fStateStack2Changed;
			char Padding_177[3];
			std::vector<ai::AIMessage,std::allocator<ai::AIMessage> > m_Messages2;
			std::vector<ai::AIMessage,std::allocator<ai::AIMessage> > m_Messages1;
			std::vector<ai::AIMessage,std::allocator<ai::AIMessage> > m_Commands;
			int m_numCurCommand;
			bool m_CommandProcessed;
			bool m_CommandStackOpen;
	};

	struct Vehicle : ComplexPhysicObj
	{
		struct WheelRuntimeInfo
		{
		  CVector m_initialPos;
		  Quaternion m_initialRot;
		  bool m_bWheelPresent;
		  ai::Wheel* m_wheel;
		};

		enum CustomWeaponControlType
		{
		  CUSTOM_WEAPON_CONTROL_NONE = 0,
		  CUSTOM_WEAPON_CONTROL_POINT = 1,
		  CUSTOM_WEAPON_CONTROL_OBJECT = 2,
		};

		enum TurningBackStatus : __int32
		{
			 TURN_BACK_NONE = 0x0,
			 TURN_BACK_ENABLED_ACCELERATING = 0x1,
			 TURN_BACK_ENABLED_BRAKING = 0x2,
			 TURN_BACK_DISABLED = 0x3,
		};

		enum VehicleMoveStatus
		{
		  MOVE_IDLE = 0,
		  MOVE_MOVING_ALONG_PATH = 1,
		  MOVE_MOVING_BY_STEERING_FORCE = 2,
		};

		enum VehicleAttackStatus
		{
		  ATTACK_IDLE = 0,
		  ATTACK_ATTACKING = 1,
		};

		std::vector<ai::Vehicle::WheelRuntimeInfo> m_wheels;
		ai::AI m_AI;
		bool m_bHorn;
		bool m_bGodMode;
		bool m_bImmortalMode;
		int32_t m_stoppageMode;
		int32_t m_onOilMode;
		int32_t m_inSmokeScreenMode;
		float m_turboThrottleTime;
		float m_turboThrottleValue;
		float m_timeAfterDeath;
		uint32_t m_numBlownParts;
		float m_timeAfterLastBlow;
		uint32_t m_shootTypeChangeTime;
		uint32_t m_shootTimeToWait;
		bool m_bIsShooting;
		stable_size_map<int,ai::Gadget*> m_gadgets;
		float m_antiMissileGadgetSavingRadius;
		float m_diffRatio;
		float m_maxEngineRpm;
		float m_lowGearShiftLimit;
		float m_highGearShiftLimit;
		float m_steeringSpeed;
		bool m_bIsTrailer;
		float m_cruisingSpeed;
		bool m_maxSpeedLimited;
		float m_maxSpeedLimit;
		bool m_maxTorqueForced;
		float m_maxTorqueForcedValue;
		int32_t m_currentGear;
		float m_throttle;
		float m_brake;
		float m_realThrottle;
		float m_engineRpm;
		float m_averageEngineRpm;
		float m_driftCoeff;
		float m_averageWheelAVel;
		bool m_bAutoBrake;
		bool m_bHandBrake;
		std::deque<float> m_recentEngineRpms;
		float m_steerRadians;
		ai::Vehicle::TurningBackStatus m_turningBackStatus;
		int32_t m_seenObjId;
		CVector m_curLookAt;
		int32_t m_npcMotionControllerId;
		std::set<ref_ptr<ai::Obstacle>> m_currentNearbyObstacles;
		std::set<ref_ptr<ai::Obstacle>> m_pastNearbyObstacles;
		CVector m_pastTakingSpherePosition;
		bool m_bAllowPickUpMessage;
		int32_t m_pastNumNearbyChests;
		int32_t m_currentNumNearbyChests;
		scoped_ptr<ai::Box> m_lookBox;
		scoped_ptr<ai::Box> m_targetBox;
		std::set<m3d::Class *,std::less<m3d::Class *>,std::allocator<m3d::Class *> > m_targetClasses;
		CVector m_externalDestination;
		int32_t m_numOfDrivenWheels;
		CVector m_bumperPoint;
		bool m_bIsControlledByPlayer;
		bool m_bIsMovingAlongExternalPath;
		int32_t m_pathIndex;
		bool m_bCanBeDistractedFromMoving;
		CVector m_size;
		CVector m_currentDestination;
		ai::Path* m_pPath;
		int m_pathNum;
		UCHAR m_priority;
		bool m_bCustomControl;
		ai::Vehicle::CustomWeaponControlType m_customControlWeapons;
		CVector m_customControlWeaponsTarget;
		int32_t m_customControlWeaponsTargetObjId;
		std::map<int,bool,std::less<int>,std::allocator<std::pair<int const ,bool> > > m_gunsPointed;
		bool m_bRocketLaunchersPresent;
		int32_t m_indexInTeam;
		ai::Vehicle::VehicleMoveStatus m_moveStatus;
		ai::Vehicle::VehicleAttackStatus m_attackStatus;
		ai::DamageType m_lastDamage;
		ai::VehiclePart* m_lastDamagedPart;
		ai::DamageType m_deathDamage;
		ai::SphereForIntersection* m_takingSphere;
		ai::IzvratRepository* m_repository;
		ai::GeomRepository* m_groundRepository;
		stable_size_vector<ActionType> m_effectActions;
		CStr m_blastEffectName;
		CStr m_destroyEffectNames[4];
		m3d::SgSoundSourceNode* m_engineHighSoundNode;
		m3d::SgSoundSourceNode* m_engineLowSoundNode;
		m3d::SgNode* m_hornSoundNode;
		float m_cameraHeight;
		float m_cameraMaxDist;
		int32_t m_trailerObjId;
		dxJoint* m_trailerJoint;
		CVector m_relTrailerJointPosOnMe;
		CVector m_relTrailerJointPosOnTrailer;
		int32_t m_recollectionId;
		ai::VehicleUpdater* m_ownUpdater;
		int32_t m_roleId;
		ai::NumericInRangeRegenerating<float> m_timeOutForNextIntersectionWithWorld;
		int32_t m_numWheelsTouchingGround;
		bool m_bHidden;
		int32_t m_lockedObjId;
		int32_t m_toBeLockedObjId;
		float m_timeToLockTarget;
		bool m_bMustGetOutOfDifficultPlace;
		bool m_bWasStuck;
		CVector m_prevPosToCheckStuck;
		float m_timeOutToCheckStuck;
		CVector m_curSteeringForce;
		bool m_bCurSteeringForceValid;
		int32_t m_soundRechargeChannelId;

		void SetGamePositionOnGround(CVector pos, bool bWithCollisions, bool bWithWater)
		{
			FUNC(0x005E09D0, void, __thiscall, _SetGamePositionOnGround, Vehicle*, CVector, bool, bool);
			return _SetGamePositionOnGround(this, pos, bWithCollisions, bWithWater);
		}

		float GetHealth()
		{
			FUNC(0x005D0BC0, float, __thiscall, _GetMaxHealth, Vehicle*);
			return _GetMaxHealth(this);
		}

		float GetMaxHealth()
		{
			FUNC(0x005D0C00, float, __thiscall, _GetMaxHealth, Vehicle*);
			return _GetMaxHealth(this);
		}

		float GetFuel()
		{
			FUNC(0x005D0C40, float, __thiscall, _GetFuel, Vehicle*);
			return _GetFuel(this);
		}

		float GetMaxFuel()
		{
			FUNC(0x005D0C80, float, __thiscall, _GetMaxFuel, Vehicle*);
			return _GetMaxFuel(this);
		}

		void ActivateHeadLights(bool bActivate)
		{
			FUNC(0x005DA130, void, __thiscall, _ActivateHeadLights, Vehicle*, bool);
			_ActivateHeadLights(this, bActivate);
		}

		void SetThrottle(float throttle, bool autoBrake)
		{
			FUNC(0x005D1210, void, __thiscall, _SetThrottle, Vehicle*, float, bool);
			_SetThrottle(this, throttle, autoBrake);
		}

		CVector* GetLinearVelocity(CVector* result)
		{
			FUNC(0x005D11C0, CVector*, __thiscall, _GetLinearVelocity, Vehicle*, CVector*);
			return _GetLinearVelocity(this, result);
		}

		const ai::VehiclePrototypeInfo* GetPrototypeInfo()
		{
			FUNC(0x005DC890, const ai::VehiclePrototypeInfo*, __thiscall, _GetPrototypeInfo, Vehicle*);
			return _GetPrototypeInfo(this);
		}

		__int32 GetMoveStatus()
		{
			FUNC(0x005CBB10, __int32, __thiscall, _GetMoveStatus, Vehicle*);
			return _GetMoveStatus(this);
		}

		float GetMaxFullDurability()
		{
			FUNC(0x0005D0D70, float, __thiscall, _GetMaxFullDurability, Vehicle*);
			return _GetMaxFullDurability(this);
		}

		float GetFullDurability()
		{
			FUNC(0x0005D0CF0, float, __thiscall, _GetFullDurability, Vehicle*);
			return _GetFullDurability(this);
		}

		Basket* GetBasket()
		{
			FUNC(0x005CBA00, ai::Basket*, __thiscall, _GetBasket, Vehicle*);
			return _GetBasket(this);
		}

		Cabin* GetCabin()
		{
			FUNC(0x005CBA00, ai::Cabin*, __thiscall, _GetCabin, Vehicle*);
			return _GetCabin(this);
		}

		Chassis* GetChassis()
		{
			FUNC(0x005CB9A0, ai::Chassis*, __thiscall, _GetChassis, Vehicle*);
			return _GetChassis(this);
		}

		CVector* _CalcSteeringForceToPathPoint(CVector* result, const CVector* point, const CVector* nextPoint)
		{
			FUNC(0x005D62E0, CVector*, __thiscall, __CalcSteeringForceToPathPoint, Vehicle*, CVector*, const CVector*, const CVector*);
			return __CalcSteeringForceToPathPoint(this, result, point, nextPoint);
		}

		bool bIsBraking()
		{
			FUNC(0x005CC250, bool, __thiscall, _bIsBraking, Vehicle*);
			return _bIsBraking(this);
		}

		float GetMass()
		{
			FUNC(0x005D5BD0, float, __thiscall, _GetMass, Vehicle*);
			return _GetMass(this);
		}

		float GetMaxTorque()
		{
			FUNC(0x005CBD10, float, __thiscall, _GetMaxTorque, Vehicle*);
			return _GetMaxTorque(this);
		}

		float GetMaxSpeed()
		{
			FUNC(0x005D1370, float, __thiscall, _GetMaxSpeed, Vehicle*);
			return _GetMaxSpeed(this);
		}

		float GetCruisingSpeed()
		{
			FUNC(0x005CC360, float, __thiscall, _GetCruisingSpeed, Vehicle*);
			return _GetCruisingSpeed(this);
		}

		ai::Wheel* GetFirstExistingWheel()
		{
			FUNC(0x005D5C60, ai::Wheel*, __thiscall, _GetFirstExistingWheel, Vehicle*);
			return _GetFirstExistingWheel(this);
		}
	};

	ASSERT_SIZE(ai::Vehicle, 0x4f4);
}