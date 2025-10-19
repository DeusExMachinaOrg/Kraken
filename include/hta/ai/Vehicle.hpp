#pragma once
#include "hta/scoped_ptr.hpp"
#include "hta/CVector2.hpp"
#include "hta/m3d/SgSoundSourceNode.hpp"
#include "hta/ai/ComplexPhysicObj.h"
#include "hta/ai/IzvratRepository.h"
#include "hta/ai/ActionType.h"
#include "hta/ai/Gadget.hpp"
#include "hta/ai/Wheel.hpp"
#include "hta/ai/Box.hpp"
#include "hta/ai/AI.hpp"
#include "Basket.h"
#include "Cabin.h"
#include "Chassis.h"
#include "Path.h"

#include <deque>

namespace ai
{
    struct VehicleUpdater;
    struct VehicleRole;
    struct VehicleRecollection;
    struct GeomRepositoryItem;
    struct Team;

    enum DamageType : int32_t {
        DAMAGE_PIERCING = 0x0000,
        DAMAGE_BLAST = 0x0001,
        DAMAGE_ENERGY = 0x0002,
        DAMAGE_WATER = 0x0003,
        DAMAGE_NUM_TYPES = 0x0004,
    };

    struct VehiclePrototypeInfo : public ComplexPhysicObjPrototypeInfo {
        struct WheelInfo {
            /* Size=0x14 */
            /* 0x0000 */ int32_t m_wheelPrototypeId;
            /* 0x0004 */ Wheel::WheelSteering m_steering;
            /* 0x0008 */ CStr m_wheelPrototypeName;
        
            WheelInfo(const WheelInfo&);
            WheelInfo(const CStr, Wheel::WheelSteering);
            void PostLoad();
            ~WheelInfo();
        };
        /* Size=0x12c */
        /* 0x0000: fields for ComplexPhysicObjPrototypeInfo */
        /* 0x0090 */ stable_size_vector<WheelInfo> m_wheelInfos;
        /* 0x00a0 */ float m_diffRatio;
        /* 0x00a4 */ float m_maxEngineRpm;
        /* 0x00a8 */ float m_lowGearShiftLimit;
        /* 0x00ac */ float m_highGearShiftLimit;
        /* 0x00b0 */ float m_selfBrakingCoeff;
        /* 0x00b4 */ float m_steeringSpeed;
        /* 0x00b8 */ int32_t m_decisionMatrixNum;
        /* 0x00bc */ float m_takingRadius;
        /* 0x00c0 */ unsigned char m_priority;
        /* 0x00c4 */ CStr m_hornSoundName;
        /* 0x00d0 */ float m_cameraHeight;
        /* 0x00d4 */ float m_cameraMaxDist;
        /* 0x00d8 */ CStr m_destroyEffectNames[4];
        /* 0x0108 */ int32_t m_blastWavePrototypeId;
        /* 0x010c */ float m_additionalWheelsHover;
        /* 0x0110 */ float m_driftCoeff;
        /* 0x0114 */ float m_pressingForce;
        /* 0x0118 */ float m_healthRegeneration;
        /* 0x011c */ float m_durabilityRegeneration;
        /* 0x0120 */ CStr m_blastWavePrototypeName;
        
        virtual void _InternalCopyFrom(const PrototypeInfo&);
        VehiclePrototypeInfo();
        virtual ~VehiclePrototypeInfo();
        virtual bool LoadFromXML(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual void PostLoad();
        virtual Obj* CreateTargetObject() const;
    };


    struct Vehicle : ComplexPhysicObj
    {
        enum TurningBackStatus : __int32 {
             TURN_BACK_NONE = 0x0,
             TURN_BACK_ENABLED_ACCELERATING = 0x1,
             TURN_BACK_ENABLED_BRAKING = 0x2,
             TURN_BACK_DISABLED = 0x3,
        };
        enum VehicleMoveStatus : int32_t {
            MOVE_IDLE = 0x0000,
            MOVE_MOVING_ALONG_PATH = 0x0001,
            MOVE_MOVING_BY_STEERING_FORCE = 0x0002,
        };
        enum VehicleAttackStatus : int32_t {
            ATTACK_IDLE = 0x0000,
            ATTACK_ATTACKING = 0x0001,
        };
        enum CustomWeaponControlType : int32_t {
            CUSTOM_WEAPON_CONTROL_NONE = 0x0000,
            CUSTOM_WEAPON_CONTROL_POINT = 0x0001,
            CUSTOM_WEAPON_CONTROL_OBJECT = 0x0002,
        };

        struct WheelRuntimeInfo {
            /* Size=0x24 */
            /* 0x0000 */ CVector m_initialPos;
            /* 0x000c */ Quaternion m_initialRot;
            /* 0x001c */ bool m_bWheelPresent;
            /* 0x0020 */ Wheel* m_wheel;

            WheelRuntimeInfo(const WheelRuntimeInfo&);
            WheelRuntimeInfo(Wheel*);
            bool IsWheelPresent() const;
            const Wheel* GetWheel() const;
            Wheel* GetWheel();
            void SetWheel(Wheel*);
        };

        /* 0x0000: fields for ComplexPhysicObj */
        /* 0x014c */ stable_size_vector<WheelRuntimeInfo> m_wheels;
        /* 0x015c */ AI m_AI;
        /* 0x01bc */ bool m_bHorn;
        /* 0x01bd */ bool m_bGodMode;
        /* 0x01be */ bool m_bImmortalMode;
        /* 0x01c0 */ int32_t m_stoppageMode;
        /* 0x01c4 */ int32_t m_onOilMode;
        /* 0x01c8 */ int32_t m_inSmokeScreenMode;
        /* 0x01cc */ float m_turboThrottleTime;
        /* 0x01d0 */ float m_turboThrottleValue;
        /* 0x01d4 */ float m_timeAfterDeath;
        /* 0x01d8 */ uint32_t m_numBlownParts;
        /* 0x01dc */ float m_timeAfterLastBlow;
        /* 0x01e0 */ uint32_t m_shootTypeChangeTime;
        /* 0x01e4 */ uint32_t m_shootTimeToWait;
        /* 0x01e8 */ bool m_bIsShooting;
        /* 0x01ec */ stable_size_map<int,Gadget*> m_gadgets;
        /* 0x01f8 */ float m_antiMissileGadgetSavingRadius;
        /* 0x01fc */ float m_diffRatio;
        /* 0x0200 */ float m_maxEngineRpm;
        /* 0x0204 */ float m_lowGearShiftLimit;
        /* 0x0208 */ float m_highGearShiftLimit;
        /* 0x020c */ float m_steeringSpeed;
        /* 0x0210 */ bool m_bIsTrailer;
        /* 0x0214 */ float m_cruisingSpeed;
        /* 0x0218 */ bool m_maxSpeedLimited;
        /* 0x021c */ float m_maxSpeedLimit;
        /* 0x0220 */ bool m_maxTorqueForced;
        /* 0x0224 */ float m_maxTorqueForcedValue;
        /* 0x0228 */ int32_t m_currentGear;
        /* 0x022c */ float m_throttle;
        /* 0x0230 */ float m_brake;
        /* 0x0234 */ float m_realThrottle;
        /* 0x0238 */ float m_engineRpm;
        /* 0x023c */ float m_averageEngineRpm;
        /* 0x0240 */ float m_driftCoeff;
        /* 0x0244 */ float m_averageWheelAVel;
        /* 0x0248 */ bool m_bAutoBrake;
        /* 0x0249 */ bool m_bHandBrake;
        /* 0x024c */ stable_size_deque<float> m_recentEngineRpms;
        /* 0x0260 */ float m_steerRadians;
        /* 0x0264 */ Vehicle::TurningBackStatus m_turningBackStatus;
        /* 0x0268 */ int32_t m_seenObjId;
        /* 0x026c */ CVector m_curLookAt;
        /* 0x0278 */ int32_t m_npcMotionControllerId;
        /* 0x027c */ stable_size_set<ref_ptr<Obstacle>> m_currentNearbyObstacles;
        /* 0x0288 */ stable_size_set<ref_ptr<Obstacle>> m_pastNearbyObstacles;
        /* 0x0294 */ CVector m_pastTakingSpherePosition;
        /* 0x02a0 */ bool m_bAllowPickUpMessage;
        /* 0x02a4 */ int32_t m_pastNumNearbyChests;
        /* 0x02a8 */ int32_t m_currentNumNearbyChests;
        /* 0x02ac */ scoped_ptr<Box> m_lookBox;
        /* 0x02b0 */ scoped_ptr<Box> m_targetBox;
        /* 0x02b4 */ stable_size_set<m3d::Class*> m_targetClasses;
        /* 0x02c0 */ CVector m_externalDestination;
        /* 0x02cc */ int32_t m_numOfDrivenWheels;
        /* 0x02d0 */ CVector m_bumperPoint;
        /* 0x02dc */ bool m_bIsControlledByPlayer;
        /* 0x02dd */ bool m_bIsMovingAlongExternalPath;
        /* 0x02e0 */ int32_t m_pathIndex;
        /* 0x02e4 */ bool m_bCanBeDistractedFromMoving;
        /* 0x02e8 */ CVector m_size;
        /* 0x02f4 */ CVector m_currentDestination;
        /* 0x0300 */ Path* m_pPath;
        /* 0x0304 */ int32_t m_pathNum;
        /* 0x0308 */ unsigned char m_priority;
        /* 0x0309 */ bool m_bCustomControl;
        /* 0x030c */ CustomWeaponControlType m_customControlWeapons;
        /* 0x0310 */ CVector m_customControlWeaponsTarget;
        /* 0x031c */ int32_t m_customControlWeaponsTargetObjId;
        /* 0x0320 */ stable_size_map<int,bool> m_gunsPointed;
        /* 0x032c */ bool m_bRocketLaunchersPresent;
        /* 0x0330 */ int32_t m_indexInTeam;
        /* 0x0334 */ VehicleMoveStatus m_moveStatus;
        /* 0x0338 */ VehicleAttackStatus m_attackStatus;
        /* 0x033c */ DamageType m_lastDamage;
        /* 0x0340 */ VehiclePart* m_lastDamagedPart;
        /* 0x0344 */ DamageType m_deathDamage;
        /* 0x0348 */ SphereForIntersection* m_takingSphere;
        /* 0x034c */ IzvratRepository* m_repository;
        /* 0x0350 */ GeomRepository* m_groundRepository;
        /* 0x0354 */ stable_size_vector<enum ActionType> m_effectActions;
        /* 0x0364 */ CStr m_blastEffectName;
        /* 0x0370 */ CStr m_destroyEffectNames[4];
        /* 0x03a0 */ m3d::SgSoundSourceNode* m_engineHighSoundNode;
        /* 0x03a4 */ m3d::SgSoundSourceNode* m_engineLowSoundNode;
        /* 0x03a8 */ m3d::SgNode* m_hornSoundNode;
        /* 0x03ac */ float m_cameraHeight;
        /* 0x03b0 */ float m_cameraMaxDist;
        /* 0x03b4 */ int32_t m_trailerObjId;
        /* 0x03b8 */ dxJoint* m_trailerJoint;
        /* 0x03bc */ CVector m_relTrailerJointPosOnMe;
        /* 0x03c8 */ CVector m_relTrailerJointPosOnTrailer;
        /* 0x03d4 */ int32_t m_recollectionId;
        /* 0x03d8 */ VehicleUpdater* m_ownUpdater;
        /* 0x03dc */ int32_t m_roleId;
        /* 0x03e0 */ NumericInRangeRegenerating<float> m_timeOutForNextIntersectionWithWorld;
        /* 0x04b8 */ int32_t m_numWheelsTouchingGround;
        /* 0x04bc */ bool m_bHidden;
        /* 0x04c0 */ int32_t m_lockedObjId;
        /* 0x04c4 */ int32_t m_toBeLockedObjId;
        /* 0x04c8 */ float m_timeToLockTarget;
        /* 0x04cc */ bool m_bMustGetOutOfDifficultPlace;
        /* 0x04cd */ bool m_bWasStuck;
        /* 0x04d0 */ CVector m_prevPosToCheckStuck;
        /* 0x04dc */ float m_timeOutToCheckStuck;
        /* 0x04e0 */ CVector m_curSteeringForce;
        /* 0x04ec */ bool m_bCurSteeringForceValid;
        /* 0x04f0 */ int32_t m_soundRechargeChannelId;
        static m3d::Class m_classVehicle;
        static stable_size_map<CStr,int> m_propertiesMap;
        static stable_size_map<int,enum eGObjPropertySaveStatus> m_propertiesSaveStatesMap;
        static const int32_t NUM_GEARS;
        static const float GEAR_RATIOS[5];
        static const float TRANSFERBOX_RATIO;
        
        virtual ~Vehicle();
        Vehicle(const VehiclePrototypeInfo&);
        Vehicle(const Vehicle&);
        virtual m3d::Object* Clone();
        virtual m3d::Class* GetClass() const;
        virtual const VehiclePrototypeInfo* GetPrototypeInfo() const;
        virtual eGObjPropertySaveStatus GetPropertySaveStatus(int32_t) const;
        virtual void GetPropertiesNames(std::set<CStr,std::less<CStr>,std::allocator<CStr> >&) const;
        virtual void GetPropertiesIDs(std::set<int,std::less<int>,std::allocator<int> >&) const;
        virtual CStr GetPropertyName(int32_t) const;
        virtual bool SetPropertyById(int32_t, const m3d::AIParam&);
        virtual int32_t GetPropertyId(const char*) const;
        virtual bool _GetPropertyDefaultInternal(int32_t, m3d::AIParam&) const;
        virtual bool _GetPropertyInternal(int32_t, m3d::AIParam&) const;
        virtual void Remove();
        virtual void LoadFromXML(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual void LoadRuntimeValues(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual void SaveToXML(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
        virtual void SaveRuntimeValues(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
        virtual void CreateChildren();
        virtual void SetBelong(int32_t);
        virtual bool CanChildBeAdded(m3d::Class*) const;
        virtual void AddChild(Obj*);
        virtual bool RemoveChild(Obj*);
        virtual void Update(float, uint32_t);
        virtual void TransferToSpace(dxSpace*);
        virtual void SetPassedToAnotherMapStatus();
        virtual bool ApplyModifier(const Modifier&);
        virtual void SetPartByName(const CStr&, VehiclePart*, bool);
        const Chassis* GetChassis() const;
        Chassis* GetChassis();
        const Cabin* GetCabin() const;
        Cabin* GetCabin();
        const Basket* GetBasket() const;
        Basket* GetBasket();
        void SetCabin(VehiclePart*);
        void SetBasket(VehiclePart*);
        uint32_t GetNumWheels() const;
        Wheel* GetWheel(uint32_t);
        const Wheel* GetWheel(uint32_t) const;
        Quaternion GetWheelInitialRotation(uint32_t);
        float GetHealth() const;
        float GetMaxHealth() const;
        float GetFuel() const;
        float GetMaxFuel() const;
        float GetControl() const;
        void ActivateHeadLights(bool);
        void GetOutOfDifficultPlace();
        virtual void InflictDamage(const DamageInfo&);
        VehicleMoveStatus GetMoveStatus() const;
        void SetMoveStatus(VehicleMoveStatus);
        VehicleAttackStatus GetAttackStatus() const;
        void SetAttackStatus(VehicleAttackStatus);
        const IzvratRepository* GetRepository() const;
        IzvratRepository* GetRepository();
        void CollectNearbyObjectsToGroundRepository();
        GeomRepository* GetGroundRepository() const;
        void PickUpNearbyObjects(bool, uint32_t&, std::vector<int,std::allocator<int> >&);
        void IntersectWithWorld() const;
        const std::set<ref_ptr<Obstacle>,std::less<ref_ptr<Obstacle> >,std::allocator<ref_ptr<Obstacle> > >& GetNearbyObstacles() const;
        void GetEnemiesInNeighborhood(float, std::vector<int,std::allocator<int> >&) const;
        void SubscribeRadioManagerOnNearbyObjId(const int32_t) const;
        void UnsubscribeRadioManagerFromNearbyObjId(const int32_t) const;
        void UnsubscribeRadioManagerFromAllNearbyObjIds() const;
        bool bIsControlledByPlayer() const;
        bool bIsMovingAlongExternalPath() const;
        void SetCustomControlEnabled(bool);
        void SetCustomControlWeapons(int32_t);
        void SetCustomControlWeapons(Vehicle::CustomWeaponControlType);
        Vehicle::CustomWeaponControlType GetCustomControlWeapons() const;
        void SetCustomControlWeaponsTarget(const CVector&);
        CVector GetCustomControlWeaponsTarget() const;
        void SetCustomControlWeaponsTargetObj(int32_t);
        int32_t GetCustomControlWeaponsTargetObj() const;
        void SetCustomLinearVelocity(float);
        int32_t GetCurrentGear() const;
        float GetEngineRpm() const;
        float GetMaxEngineRpm() const;
        float GetAverageEngineRpm() const;
        float GetMaxPower() const;
        void SetMaxPower(float);
        float GetMaxTorque() const;
        void SetMaxTorque(float);
        int32_t GetMaxGadgets(const CStr&) const;
        const stable_size_map<int,Gadget*>& GetGadgets() const;
        int32_t GetValidSlotIdForGadget(const Gadget*) const;
        bool AddGadget(Gadget*);
        void RecalcGadgets();
        const NumericInRangeRegenerating<float>& Health() const;
        NumericInRangeRegenerating<float>& Health();
        const NumericInRangeRegenerating<float>& Fuel() const;
        NumericInRangeRegenerating<float>& Fuel();
        float GetFullDurability() const;
        float GetMaxFullDurability() const;
        float GetFullDurabilityCoeffForDamageType(DamageType) const;
        void WeaponLookAtPoint(const CVector&, float);
        void FireFromWeaponCustom(bool, const CVector&, Obj*);
        void FireFromWeaponCustom2(bool, int32_t);
        void FireFromWeaponAI(bool, float, Obj*);
        void HoldFire(int32_t);
        float EstimateDamageAI(const CVector&, std::vector<int,std::allocator<int> >) const;
        float EstimateDamageAI() const;
        float EstimateDamageFromPositionAI(const CVector&, const CVector&, std::vector<int,std::allocator<int> >) const;
        float GetMaxFiringRangeAI() const;
        bool FireFromWeaponByGunId(int32_t, bool);
        bool FireFromWeaponByGunPartName(const CStr&, bool);
        bool DriveToPoint(const CVector&, const CVector&, bool, float);
        virtual float GetMass() const;
        CVector GetSize() const;
        virtual void SetPositionSelf(const CVector&);
        virtual void SetRotationSelf(const Quaternion&);
        void SetGamePositionOnGround(const CVector&, bool, bool);
        virtual CVector GetGeometricCenter() const;
        virtual CVector GetLinearVelocity() const;
        virtual void SetLinearVelocity(const CVector&);
        void ResetPositionAndRotation();
        float GetCameraHeight() const;
        float GetCameraMaxDist() const;
        int32_t GetIndexInTeam() const;
        void SetIndexInTeam(int32_t);
        Team* GetTeam() const;
        float GetThrottle() const;
        void SetThrottle(float, bool);
        bool bIsBraking() const;
        float GetBrake() const;
        void SetBrake(float);
        void SetHandBrake();
        void ReleaseAllPedals();
        float GetSteer() const;
        void SetSteer(float);
        float GetCurrentSteerAngle() const;
        unsigned char GetPriority() const;
        float GetMaxSpeed() const;
        void SetMaxSpeed(float);
        float GetCruisingSpeed() const;
        void SetCruisingSpeed(float);
        float GetDefaultCruisingSpeed() const;
        void LimitMaxSpeed(float);
        void UnlimitMaxSpeed();
        void SetForcedMaxTorque(float);
        void ResetForcedMaxTorque();
        void SetTurningToGroundForceAndTorque(const CVector&, const CVector&, const CVector&);
        CVector GetBumperPoint() const;
        const Wheel* GetFirstExistingWheel() const;
        virtual void RenderDebugInfo() const;
        virtual void Flow(Obj*, float);
        virtual void Blow(Obj*);
        int32_t SetExternalPath(const stable_size_vector<CVector2>&);
        int32_t SetExternalPathByName(const char*);
        void SetCanBeDistractedFromMoving(bool);
        void SetExternalDestination(const CVector&);
        void PlaceToEndOfPath();
        void SetTrailer();
        bool IsTrailer() const;
        bool AddThing(const GeomRepositoryItem&, bool);
        bool AddItemsToRepository(const char*, int32_t);
        bool RemoveItemsFromRepository(const char*, int32_t);
        bool HasAmountOfItemsInRepository(const char*, int32_t) const;
        bool CanPlaceItemsToRepository(const char*, int32_t);
        bool AddObjectToRepository(Obj*);
        m3d::AIParam TakeOffAllGuns();
        void AttachTrailer(const char*);
        void DetachTrailer();
        bool TrailerExists() const;
        Vehicle* GetTrailer() const;
        virtual void SetUpdatingByODE(bool);
        VehicleRecollection* GetRecollection() const;
        CVector GetRecollectionPosition(float) const;
        float GetCollisionRadius() const;
        void IncNumWheelsTouchingGround();
        virtual void SetSkin(int32_t);
        virtual void SetRandomSkin();
        virtual uint32_t GetPrice(const IPriceCoeffProvider*) const;
        virtual uint32_t GetSchwarz() const;
        bool GetHorn() const;
        void SetHorn(bool);
        void HealWheels();
        virtual void SetVisible();
        virtual void SetInvisible();
        bool getGodMode() const;
        void setGodMode(bool);
        bool getImmortalMode() const;
        void setImmortalMode(bool);
        bool GetStoppageMode() const;
        void IncStoppageMode();
        void DecStoppageMode();
        bool GetOnOilMode() const;
        void IncOnOilMode();
        void DecOnOilMode();
        bool GetInSmokeScreenMode() const;
        void IncInSmokeScreenMode();
        void DecInSmokeScreenMode();
        float GetTurboThrottleTime() const;
        void SetTurboThrottleTime(float);
        float GetTurboThrottleValue() const;
        void SetTurboThrottleValue(float);
        virtual void GetGeoms(std::vector<Geom *,std::allocator<Geom *> >&) const;
        virtual Obj* CloneObj();
        virtual void ClearSavedStatus();
        float GetDriftCoeff() const;
        void ShowVehicle(bool);
        virtual void DisablePhysics();
        virtual void EnablePhysics();
        virtual void DisableGeometry(bool);
        virtual void EnableGeometry(bool);
        VehicleRole* GetRole() const;
        void SetRole(VehicleRole*);
        int32_t GetNpcMotionControllerId() const;
        void SetNpcMotionControllerId(int32_t);
        int32_t GetSeenObjId() const;
        int32_t GetInfoObjId() const;
        int32_t GetLockedObjId() const;
        int32_t GetToBeLockedObjId() const;
        float GetTimeToLockTarget() const;
        bool bRocketLaunchersPresent() const;
        void EnableSounds(bool);
        bool IsHealthZero() const;
        void PlaySoundOnRechargeWeapon();
        int32_t CheckSkin(int32_t);
        virtual void _InternalPostLoad();
        virtual void _InternalCreateVisualPart();
        virtual AI* GetAIPtr();
        virtual void _KeepSteer(float);
        virtual void _UpdateOwnPhysics(float);
        virtual void _PutContour();
        virtual void _RemoveContour();
        virtual float _CalcMassForBody() const;
        float _GetTimeOutForNextIntersectionWithWorld() const;
        void _DropChests();
        void _EvaluateToDead();
        void _DeadActions(float);
        int32_t _UpdateRepositoryOnChangeBasket();
        void _ValidateVehicleParts();
        void _OnChangeCabin();
        void _OnChangeBasket();
        void _CheckForNearbyChests() const;
        void _CauseCustomGunPointedEvents();
        CVector _GetCustomWeaponTargetPoint() const;
        float _GetAngleTo(const CVector&) const;
        CVector _GetNextPathPoint() const;
        CVector _GetLastPathPoint() const;
        void _AdjustSizeAndBumperPoint();
        void _AdjustWheel(WheelRuntimeInfo&);
        void _TurnWheelByAngle(Wheel*, float);
        void _DriveBySteeringForce(const CVector&);
        bool _bPassedPathPoint(const CVector&, const CVector&, bool) const;
        bool _bPointIsBehind(const CVector&) const;
        void _KeepThrottle(bool);
        void _KeepGearBox(float);
        void _KeepSuspension();
        void _ApplyStabilizingForces();
        CVector _CalcSteeringForce(float) const;
        CVector _CalcSteeringForceToPathPoint(const CVector&, const CVector&) const;
        CVector _CalcRepulsionForNearbyObjects(const CVector&, const CVector&, const CVector&, const CVector&, bool, CVector&) const;
        CVector _CalcRepulsionForObstacle(const Obstacle*, const CVector&, const CVector&, const CVector&, const CVector&, bool, CVector&) const;
        void _AdjustLookBox(bool, const CVector&, const CVector&, const CVector&) const;
        void _UpdateAlarmStatus();
        CVector _GetEtalonWheelAVel() const;
        void _CalcRpms();
        CStr _GetTrailerName() const;
        void _AttachExistingTrailer(Vehicle*, bool);
        void _AdjustTrailer();
        void _TakeWaterIntoAccount(float);
        void _UpdateSeenObjAndWeapons(float);
        void _UpdateInfoObj();
        void _UpdateLockedObj(float);
        void _UpdatePhysicsUpdater();
        void _InflictDamageToRepository(float);
        void _CreateBlastWave();
        void _EnsureRecollection();
        bool _SetIdleMoveStatus();
        void _SetIdleMoveStatusAndCauseTargetReached();
        void _GetOutOfDifficlultPlaceInternal();
        float _GetCabinControlCoeff() const;

        static m3d::Object* CreateObject();
        static m3d::Class* GetBaseClass();
        static void RegisterProperty(const char*, int32_t, eGObjPropertySaveStatus);
        static void Registration();
        static m3d::AIParam VehicleAIOnDefend(Obj*);
        static m3d::AIParam VehicleAIOnMove(Obj*);
        static m3d::AIParam VehicleAIOnAttack(Obj*);
        static m3d::AIParam VehicleAIOnDead(Obj*);
        static const char* MoveStatusName(VehicleMoveStatus);
        static const char* AttackStatusName(VehicleAttackStatus);
        static VehicleMoveStatus MoveStatusID(const CStr&);
        static VehicleAttackStatus AttackStatusID(const CStr&);
    };
    ASSERT_SIZE(Vehicle, 0x4f4);
};