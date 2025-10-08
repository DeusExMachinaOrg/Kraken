#pragma once
#include <deque>
#include "ComplexPhysicObj.h"
#include "IzvratRepository.h"
#include "ActionType.h"
#include "VehiclePrototypeInfo.h"
#include "Basket.h"
#include "Cabin.h"
#include "Chassis.h"
#include "Path.h"
#include "ref_ptr.h"
#include "scoped_ptr.h"
#include "Geom.h"

class dxGeom;
class dxJoint;

namespace m3d
{
	namespace cmn
	{
		class XmlFile;
		class XmlNode;
	}

	class Class;

	class SgSoundSourceNode;
}

namespace ai
{
	class VehicleUpdater;

	enum DamageType
	{
		DAMAGE_PIERCING = 0,
		DAMAGE_BLAST = 1,
		DAMAGE_ENERGY = 2,
		DAMAGE_WATER = 3,
		DAMAGE_NUM_TYPES = 4,
	};

	class SphereForIntersection;

	class Box : Geom { };

	class Obstacle
	{
  		LONG m_refCount;
  		BOOLEAN m_bIsEnabled;
  		void* m_intersectionSphere;
  		void* m_intersectionBox;
  		LONG m_ownerPhysicObjId;
  		m3d::SgNode* m_ownerSgNode;
	};

	class AIPassageState
	{
		public:
			int m_StateNum;
			std::vector<m3d::AIParam,std::allocator<m3d::AIParam> > m_ParamList;
			void LoadFromXML(m3d::cmn::XmlFile* xmlFile, const m3d::cmn::XmlNode* OwnNode);
			void SaveToXML(m3d::cmn::XmlFile* xmlFile, m3d::cmn::XmlNode* OwnNode) const;
	};

	class DecisionMatrix;

	class AIMessage
	{
		public:
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

	class AI
	{
		public:
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

	struct Wheel;
	struct Gadget;
	struct Vehicle : ComplexPhysicObj
	{
		struct WheelRuntimeInfo
		{
		  CVector m_initialPos;
		  Quaternion m_initialRot;
		  BOOLEAN m_bWheelPresent;
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
		BOOLEAN m_bHorn;
		BOOLEAN m_bGodMode;
		BOOLEAN m_bImmortalMode;
		LONG m_stoppageMode;
		LONG m_onOilMode;
		LONG m_inSmokeScreenMode;
		FLOAT m_turboThrottleTime;
		FLOAT m_turboThrottleValue;
		FLOAT m_timeAfterDeath;
		ULONG m_numBlownParts;
		FLOAT m_timeAfterLastBlow;
		ULONG m_shootTypeChangeTime;
		ULONG m_shootTimeToWait;
		BOOLEAN m_bIsShooting;
		stable_size_map<int,ai::Gadget*> m_gadgets;
		FLOAT m_antiMissileGadgetSavingRadius;
		FLOAT m_diffRatio;
		FLOAT m_maxEngineRpm;
		FLOAT m_lowGearShiftLimit;
		FLOAT m_highGearShiftLimit;
		FLOAT m_steeringSpeed;
		BOOLEAN m_bIsTrailer;
		FLOAT m_cruisingSpeed;
		BOOLEAN m_maxSpeedLimited;
		FLOAT m_maxSpeedLimit;
		BOOLEAN m_maxTorqueForced;
		FLOAT m_maxTorqueForcedValue;
		LONG m_currentGear;
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
		FLOAT m_steerRadians;
		ai::Vehicle::TurningBackStatus m_turningBackStatus;
		LONG m_seenObjId;
		CVector m_curLookAt;
		LONG m_npcMotionControllerId;
		std::set<ref_ptr<ai::Obstacle>> m_currentNearbyObstacles;
		std::set<ref_ptr<ai::Obstacle>> m_pastNearbyObstacles;
		CVector m_pastTakingSpherePosition;
		BOOLEAN m_bAllowPickUpMessage;
		LONG m_pastNumNearbyChests;
		LONG m_currentNumNearbyChests;
		scoped_ptr<ai::Box> m_lookBox;
		scoped_ptr<ai::Box> m_targetBox;
		std::set<m3d::Class *,std::less<m3d::Class *>,std::allocator<m3d::Class *> > m_targetClasses;
		CVector m_externalDestination;
		LONG m_numOfDrivenWheels;
		CVector m_bumperPoint;
		BOOLEAN m_bIsControlledByPlayer;
		BOOLEAN m_bIsMovingAlongExternalPath;
		LONG m_pathIndex;
		BOOLEAN m_bCanBeDistractedFromMoving;
		CVector m_size;
		CVector m_currentDestination;
		ai::Path* m_pPath;
		int m_pathNum;
		UCHAR m_priority;
		BOOLEAN m_bCustomControl;
		ai::Vehicle::CustomWeaponControlType m_customControlWeapons;
		CVector m_customControlWeaponsTarget;
		LONG m_customControlWeaponsTargetObjId;
		std::map<int,bool,std::less<int>,std::allocator<std::pair<int const ,bool> > > m_gunsPointed;
		BOOLEAN m_bRocketLaunchersPresent;
		LONG m_indexInTeam;
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
		FLOAT m_cameraHeight;
		FLOAT m_cameraMaxDist;
		LONG m_trailerObjId;
		dxJoint* m_trailerJoint;
		CVector m_relTrailerJointPosOnMe;
		CVector m_relTrailerJointPosOnTrailer;
		LONG m_recollectionId;
		ai::VehicleUpdater* m_ownUpdater;
		LONG m_roleId;
		ai::NumericInRangeRegenerating<float> m_timeOutForNextIntersectionWithWorld;
		LONG m_numWheelsTouchingGround;
		BOOLEAN m_bHidden;
		LONG m_lockedObjId;
		LONG m_toBeLockedObjId;
		FLOAT m_timeToLockTarget;
		BOOLEAN m_bMustGetOutOfDifficultPlace;
		BOOLEAN m_bWasStuck;
		CVector m_prevPosToCheckStuck;
		FLOAT m_timeOutToCheckStuck;
		CVector m_curSteeringForce;
		BOOLEAN m_bCurSteeringForceValid;
		LONG m_soundRechargeChannelId;

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
	}; 
}
ASSERT_SIZE(ai::Vehicle, 0x4f4);