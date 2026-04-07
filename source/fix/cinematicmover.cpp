#define LOGGER "cinematicmover"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

#include "fix/cinematicmover.hpp"
#include "ext/logger.hpp"
#include "routines.hpp"

#include "hta/CStr.hpp"
#include "hta/CVector.hpp"
#include "hta/Quaternion.hpp"
#include "hta/ai/CServer.hpp"
#include "hta/ai/CinematicMover.hpp"
#include "hta/ai/ObjContainer.hpp"
#include "hta/ai/PhysicBody.hpp"
#include "hta/ai/PhysicObj.hpp"
#include "hta/ai/GlobalProperties.hpp"
#include "hta/m3d/Application.hpp"
#include "hta/m3d/Cinematic.hpp"
#include "hta/m3d/Class.hpp"
#include "hta/m3d/CameraPath.hpp"
#include "hta/m3d/CameraPathState.hpp"
#include "hta/m3d/Kernel.hpp"
#include "hta/m3d/cmn/XmlFile.hpp"

    struct dObject {
        dxWorld*  world;
        dObject*  next;
        dObject** tome;
        void*     userdata;
        int32_t   tag;
    };

    struct dxGeom {};

    struct dxAutoDisable {
        float linear_threshold;
        float angular_threshold;
        float idle_time;
        int   idle_steps;
    };

    struct dxDamping {
        float m_linearDamping;
        float m_angularDamping;
    };

    enum MovingMode : __int32
    {
        MOVE_IN_UPDATE         = 0x0,
        MOVE_BEFORE_PHYSICSTEP = 0x1,
    };

    const hta::CVector ZeroVector(0.0f, 0.0f, 0.0f);

    struct dxBody: dObject {
        dxJointNode*  firstjoint;
        int32_t       flags;
        dxGeom*       geom;
        dMass         mass;
        float         invI[12];
        float         invMass;
        float         pos[4];
        float         q[4];
        float         R[12];
        float         lvel[4];
        float         avel[4];
        float         facc[4];
        float         tacc[4];
        float         finite_rot_axis[4];
        dxAutoDisable adis;
        dxDamping     m_damping;
        float         adis_timeleft;
        int           adis_stepsleft;
        void (__fastcall* m_movedCallback)(dxBody *);
        void (__fastcall* m_changeEnabledStateCallback)(dxBody *);
    };

using namespace hta;

namespace kraken::fix::cinematicmover {
    namespace {
        struct MeridianState {
            int32_t movingMode = 0;
            bool oldObjectCinematicMode = false;
            bool oldObjectGravityMode = false;
        };

        static void SetCorrectPosToControlledObj(hta::ai::CinematicMover* self, hta::ai::PhysicObj* controlledObj, float stepTime);
        static void __fastcall ControlledObjBeforeStepCallback(dxBody* body);
        static int32_t __fastcall Hooked_CollidePOAndWater(hta::m3d::Object* obj1, hta::m3d::Object* obj2, dContact* contacts, uint32_t& numContacts, bool reverse);

        using PhysicObjVoidIntFn = void(__thiscall*)(hta::ai::PhysicObj*, int32_t);
        using PhysicObjGetIntConstFn = int32_t(__thiscall*)(const hta::ai::PhysicObj*);

        static std::unordered_map<const hta::ai::PhysicObj*, int32_t> g_cinematicMoverIds;
        static std::unordered_map<const dxBody*, void*> g_beforeStepCallbacks;
        static std::unordered_map<const dxBody*, void*> g_savedBeforeStepCallbacks;
        static float g_beforeStepDeltaTime = 1.0f / 120.0f;

        CUSTOM static bool CanCreateCollisionEffect2(const hta::ai::PhysicObj* obj) {
            if (!obj) {
                return false;
            }

            return obj->m_timeFromLastCollisionEffect > 0.1f;
        }

        static bool PhysicObj_IsCinematic(const hta::ai::PhysicObj* obj) {
            if (!obj) {
                return false;
            }

            const auto* body = obj->GetBody() ? obj->GetBody()->_id : nullptr;
            if (!body) {
                return false;
            }

            // Meridian dBodyIsCinematic: return (body->flags >> 15) & 1.
            return ((static_cast<uint32_t>(body->flags) >> 15) & 1u) != 0;
        }

        static void PhysicObj_SetCinematic(hta::ai::PhysicObj* obj, bool value) {
            if (!obj) {
                return;
            }

            auto* body = obj->GetBody() ? obj->GetBody()->_id : nullptr;
            if (!body) {
                return;
            }

            constexpr uint32_t cinematicMask = 1u << 15;
            if (value) {
                body->flags |= static_cast<int32_t>(cinematicMask);
            } else {
                body->flags &= ~static_cast<int32_t>(cinematicMask);
            }
        }

        static bool PhysicObj_GetGravityMode(const hta::ai::PhysicObj* obj) {
            if (!obj) {
                return true;
            }

            return reinterpret_cast<bool(__fastcall*)(const dxBody*)>(0x007C4C00)(obj->GetBody()->_id) != 0;
        }

        static bool PhysicObj_IsPhysicsEnabled(const hta::ai::PhysicObj* obj) {
            if (!obj) {
                return false;
            }

            return reinterpret_cast<bool(__fastcall*)(const dxBody*)>(0x007C4BE0)(obj->GetBody()->_id) != 0;
        }

        static void PhysicObj_SetGravityMode(hta::ai::PhysicObj* obj, bool value) {
            if (!obj) {
                return;
            }

            dxBody* rawBody = obj->GetBody() ? obj->GetBody()->_id : nullptr;
            if (!rawBody) {
                return;
            }

            constexpr uint32_t gravityDisabledMask = 1u << 3;
            if (value) {
                rawBody->flags &= ~static_cast<int32_t>(gravityDisabledMask);
            } else {
                rawBody->flags |= static_cast<int32_t>(gravityDisabledMask);
            }
        }

        static void PhysicObj_SetCinematicMoverId(hta::ai::PhysicObj* obj, int32_t value) {
            if (!obj) {
                return;
            }

            if (value < 0) {
                g_cinematicMoverIds.erase(obj);
                return;
            }

            g_cinematicMoverIds[obj] = value;
        }

        static int32_t PhysicObj_GetCinematicMoverId(const hta::ai::PhysicObj* obj) {
            if (!obj) {
                return -1;
            }

            const auto it = g_cinematicMoverIds.find(obj);
            return it != g_cinematicMoverIds.end() ? it->second : -1;
        }

        static void* dBody_GetData(dxBody* body) {
            return body ? reinterpret_cast<void*(__fastcall*)(dxBody*)>(0x007C4580)(body) : nullptr;
        }

        static void* dBody_GetBeforeStepCallback(dxBody* body) {
            if (!body) {
                return nullptr;
            }

            const auto it = g_beforeStepCallbacks.find(body);
            return it != g_beforeStepCallbacks.end() ? it->second : nullptr;
        }

        static void dBody_SetBeforeStepCallback(dxBody* body, void* callback) {
            if (!body) {
                return;
            }

            if (callback) {
                g_beforeStepCallbacks[body] = callback;
            } else {
                g_beforeStepCallbacks.erase(body);
            }
        }

        static std::unordered_map<const hta::ai::CinematicMover*, MeridianState> g_states;

        static bool TryParseIntAttribute(const hta::m3d::cmn::XmlNode* node, const char* key, int32_t& outValue) {
            if (!node || !key) {
                return false;
            }

            const char* value = node->GetAttribute(key);
            if (!value || !*value) {
                return false;
            }

            char* endPtr = nullptr;
            const long parsed = std::strtol(value, &endPtr, 10);
            if (endPtr == value) {
                return false;
            }

            outValue = static_cast<int32_t>(parsed);
            return true;
        }

        static bool TryParseBoolAttribute(const hta::m3d::cmn::XmlNode* node, const char* key, bool& outValue) {
            int32_t parsed = 0;
            if (!TryParseIntAttribute(node, key, parsed)) {
                return false;
            }

            outValue = parsed != 0;
            return true;
        }

        static void WriteIntAttribute(hta::m3d::cmn::XmlNode* node, const char* key, int32_t value) {
            if (!node || !key) {
                return;
            }

            const hta::CStr text(value);
            node->SetAttribute(key, text.c_str());
        }

        static void WriteBoolAttribute(hta::m3d::cmn::XmlNode* node, const char* key, bool value) {
            WriteIntAttribute(node, key, value ? 1 : 0);
        }

        static MeridianState& GetState(const hta::ai::CinematicMover* self) {
            return g_states[self];
        }

        static hta::m3d::Cinematic* GetCinematic() {
            hta::m3d::Application* application = hta::m3d::Application::Instance();
            if (!application) {
                return nullptr;
            }

            return application->m_cinematic;
        }

        static void ReleaseCurrentFlyPath(hta::ai::CinematicMover* self) {
            if (!self || !self->m_currentFlyPath) {
                return;
            }

            self->m_currentFlyPath->clear();

            self->m_currentFlyPath = nullptr;
        }

        static hta::m3d::CameraPath* EnsureCurrentFlyPath(hta::ai::CinematicMover* self) {
            if (self->m_currentFlyPath) {
                return self->m_currentFlyPath;
            }

            hta::m3d::Kernel* kernel = hta::m3d::Kernel::Instance();
            if (!kernel || !kernel->m_memMan) {
                return nullptr;
            }

            auto* path = static_cast<hta::m3d::CameraPath*>(kernel->m_memMan->Malloc(static_cast<int32_t>(sizeof(hta::m3d::CameraPath)), nullptr, 0));
            if (!path) {
                return nullptr;
            }

            auto* ctor = reinterpret_cast<void(__thiscall*)(hta::m3d::CameraPath*)>(0x00629E00);
            ctor(path);
            self->m_currentFlyPath = path;
            return path;
        }

        static bool SamplePath(const hta::m3d::CameraPath* path, float time, hta::CVector& position, hta::Quaternion& rotation, float& zoom) {
            if (!path || path->empty()) {
                return false;
            }

            path->GetCameraForTime(time, position, rotation, zoom);
            return true;
        }

        REIMPL void CinematicMover_AttachControlledObj(hta::ai::CinematicMover* self)
        {
            if (self->m_controlledObjId >= 0) {
                const hta::ai::CServer* server = hta::ai::CServer::Instance();
                ai::PhysicObj* controlledObj = (ai::PhysicObj*)server->m_pObjects->GetEntityByObjId(self->m_controlledObjId);

                if (controlledObj != nullptr)
                {
                    MeridianState& state = GetState(self);
                    if (state.movingMode == MOVE_BEFORE_PHYSICSTEP)
                    {
                        state.oldObjectCinematicMode = PhysicObj_IsCinematic(controlledObj);
                        state.oldObjectGravityMode = PhysicObj_GetGravityMode(controlledObj);

                        PhysicObj_SetCinematic(controlledObj, true);
                        PhysicObj_SetGravityMode(controlledObj, false);

                        if (controlledObj->GetBody()) {
                            dxBody* rawBody = controlledObj->GetBody()->_id;
                            g_savedBeforeStepCallbacks[rawBody] = dBody_GetBeforeStepCallback(rawBody);
                            dBody_SetBeforeStepCallback(rawBody, reinterpret_cast<void*>(&ControlledObjBeforeStepCallback));
                        }
                    }
                }
            }
        }

        REIMPL void _DetachControlledObj(ai::CinematicMover* self)
        {
            if (self->m_controlledObjId >= 0)
            {
                const ai::CServer* server = ai::CServer::Instance();
                ai::PhysicObj* controlledObj = (ai::PhysicObj*)server->m_pObjects->GetEntityByObjId(self->m_controlledObjId);

                if (controlledObj != nullptr)
                {
                    MeridianState& state = GetState(self);
                    PhysicObj_SetCinematicMoverId(controlledObj, -1);

                    if (state.movingMode == MOVE_BEFORE_PHYSICSTEP)
                    {
                        dxBody* rawBody = controlledObj->GetBody()->_id;
                        const auto it = g_savedBeforeStepCallbacks.find(rawBody);
                        dBody_SetBeforeStepCallback(rawBody, it != g_savedBeforeStepCallbacks.end() ? it->second : nullptr);
                        g_savedBeforeStepCallbacks.erase(rawBody);

                        if (PhysicObj_IsCinematic(controlledObj))
                        {
                            controlledObj->SetLinearVelocity(ZeroVector);
                            controlledObj->SetAngularVelocity(ZeroVector);
                        }

                        PhysicObj_SetCinematic(controlledObj, state.oldObjectCinematicMode);
                        PhysicObj_SetGravityMode(controlledObj, state.oldObjectGravityMode);
                    }
                }
            }
        }

        static void __fastcall ControlledObjBeforeStepCallback(dxBody* body) {
            auto* controlledObj = static_cast<hta::ai::PhysicObj*>(dBody_GetData(body));
            if (!controlledObj) {
                return;
            }

            const int32_t moverId = PhysicObj_GetCinematicMoverId(controlledObj);
            if (moverId < 0) {
                return;
            }

            const hta::ai::CServer* server = hta::ai::CServer::Instance();
            if (!server || !server->m_pObjects) {
                return;
            }

            hta::ai::Obj* obj = server->m_pObjects->GetEntityByObjId(moverId);
            if (!obj || !obj->GetClass()->IsKindOf(hta::ai::CinematicMover::GetBaseClass())) {
                return;
            }

            auto* mover = static_cast<hta::ai::CinematicMover*>(obj);
            (void)GetState(mover);

            const float callbackStep = g_beforeStepDeltaTime;
            SetCorrectPosToControlledObj(mover, controlledObj, callbackStep);
            mover->m_currentFlyTime += callbackStep;

            if (mover->m_currentFlyPath && mover->m_currentFlyTime >= mover->m_currentFlyPath->GetFullTime()) {
                PhysicObj_SetCinematicMoverId(controlledObj, -1);
                PhysicObj_SetCinematic(controlledObj, false);

                if (controlledObj->GetBody()) {
                    dxBody* rawBody = controlledObj->GetBody()->_id;
                    const auto it = g_savedBeforeStepCallbacks.find(rawBody);
                    dBody_SetBeforeStepCallback(rawBody, it != g_savedBeforeStepCallbacks.end() ? it->second : nullptr);
                    g_savedBeforeStepCallbacks.erase(rawBody);
                }

                return;
            }

            const auto savedIt = g_savedBeforeStepCallbacks.find(body);
            const void* oldBeforeStepCallback = savedIt != g_savedBeforeStepCallbacks.end() ? savedIt->second : nullptr;
            if (oldBeforeStepCallback && oldBeforeStepCallback != reinterpret_cast<void*>(&ControlledObjBeforeStepCallback)) {
                reinterpret_cast<void(__fastcall*)(dxBody*)>(const_cast<void*>(oldBeforeStepCallback))(body);
            }
        }

        static int32_t __fastcall Hooked_CollidePOAndWater(hta::m3d::Object* obj1, hta::m3d::Object* obj2, dContact* contacts, uint32_t& numContacts, bool reverse) {
            (void)numContacts;
            (void)reverse;

            if (!obj1 || !obj2 || !contacts) {
                return 0;
            }

            hta::ai::PhysicObj* physicObj = nullptr;
            if (obj1->GetClass() && obj1->GetClass()->IsKindOf(hta::ai::PhysicObj::GetBaseClass())) {
                physicObj = static_cast<hta::ai::PhysicObj*>(obj1);
            } else if (obj2->GetClass() && obj2->GetClass()->IsKindOf(hta::ai::PhysicObj::GetBaseClass())) {
                physicObj = static_cast<hta::ai::PhysicObj*>(obj2);
            }

            if (!physicObj) {
                return 0;
            }

            const CVector velocity = physicObj->GetLinearVelocity();
            constexpr float kMinSplashSpeed = 1.0f;
            if (velocity.LengthSquare() < kMinSplashSpeed * kMinSplashSpeed) {
                return 0;
            }

            if (!CanCreateCollisionEffect2(physicObj)) {
                return 0;
            }

            const CVector contactPos(
                static_cast<float>(contacts->geom.pos[0]),
                static_cast<float>(contacts->geom.pos[1]),
                static_cast<float>(contacts->geom.pos[2]));

            Quaternion splashRotation;
            splashRotation.Identity();
            const CStr splashName("ET_PS_SPLINTER_WATERSPLASH");
            hta::ai::PhysicBody::CreateEffectNode(splashName, contactPos, splashRotation, true, 1.0f);
            physicObj->SetCollisionEffectCreated();

            return 0;
        }

        REIMPL static void SetCorrectPosToControlledObj(hta::ai::CinematicMover* self, hta::ai::PhysicObj* controlledObj, float stepTime) {
            hta::CVector pos = controlledObj->GetPosition();
            hta::Quaternion rot = controlledObj->GetRotation();
            float zoom;

            self->m_currentFlyPath->GetCameraForTime(self->m_currentFlyTime, pos, rot, zoom);

            controlledObj->SetPosition(pos);
            controlledObj->SetRotation(rot);

            CVector linearVel(0.0f, 0.0f, 0.0f);
            CVector angularVel(0.0f, 0.0f, 0.0f);

            const float dt = (stepTime > 0.0f && stepTime < 1.0f) ? stepTime : 0.01f;
            const float invDt = 1.0f / dt;

            if (self->m_currentFlyTime > dt) {
                CVector oldPos = pos;
                Quaternion oldRot = rot;

                self->m_currentFlyPath->GetCameraForTime(self->m_currentFlyTime - dt, oldPos, oldRot, zoom);

                linearVel = (pos - oldPos) * invDt;

                CMatrix matCurrent, matOld;
                matCurrent.Rotation(rot);
                matOld.Rotation(oldRot);

                CVector dirCurrent, dirOld, dummyX, dummyY;
                matCurrent.GetBasis(dummyX, dummyY, dirCurrent);
                matOld.GetBasis(dummyX, dummyY, dirOld);

                angularVel = CVector::Cross(dirOld, dirCurrent) * invDt;
            }

            controlledObj->SetLinearVelocity(linearVel);
            controlledObj->SetAngularVelocity(angularVel);
        }
    }

    void OnBodyBeforeStep(void* body, float stepTime) {
        if (!body) {
            return;
        }

        if (stepTime > 0.0f && stepTime < 1.0f) {
            g_beforeStepDeltaTime = stepTime;
        }

        dxBody* typedBody = static_cast<dxBody*>(body);
        const auto it = g_beforeStepCallbacks.find(typedBody);
        if (it == g_beforeStepCallbacks.end() || !it->second) {
            return;
        }

        reinterpret_cast<void(__fastcall*)(dxBody*)>(it->second)(typedBody);
    }

    static void __fastcall Hooked_ObjRemove(hta::ai::Obj* self, void*) {
        if (!self) {
            return;
        }

        if (self->GetClass() && self->GetClass()->IsKindOf(hta::ai::CinematicMover::GetBaseClass())) {
            auto* mover = static_cast<hta::ai::CinematicMover*>(self);
            if (hta::ai::PhysicObj* controlledObj = mover->_GetControlledObj()) {
                _DetachControlledObj(mover);
            }
            g_states.erase(mover);
        }

        self->_SetDeadStatus();
        self->m_flags |= 2u;
        hta::ai::CServer* server = hta::ai::CServer::Instance();
        hta::ai::ObjContainer* objcont = server ? server->m_pObjects : nullptr;
        if (objcont && self->m_objId != -1) {
            objcont->AddObjIdToRemove(self->m_objId);
        }
    }

    REIMPL int SafeStrAttrib(CStr* v, m3d::cmn::XmlNode* node, const char* attrib)
    {
        if (node->IsEmpty()) {
            return 0;
        }

        const char* attrValue = node->GetAttribute(attrib);
        if (attrValue == nullptr) {
            return 0;
        }

        CStr tempStr(attrValue);

        *v = tempStr;

        if (tempStr.m_charPtr != tempStr.ZERO)
        {
            m3d::Kernel::Instance()->g_mar.FreeMem(tempStr.m_charPtr, 0, 0);
        }

        return 1;
    }

    REIMPL bool SafeBoolAttrib(bool* v, m3d::cmn::XmlNode* node, const char* attrib)
    {
        if (node->IsEmpty()) {
            return false;
        }

        const char* attrValue = node->GetAttribute(attrib);
        if (attrValue == nullptr) {
            return false;
        }

        bool isTrue = (_stricmp(attrValue, "true") == 0) ||
                    (_stricmp(attrValue, "1") == 0) ||
                    (_stricmp(attrValue, "yes") == 0) ||
                    (_stricmp(attrValue, "yeah") == 0) ||
                    (_stricmp(attrValue, "yep") == 0);

        bool isFalse = (_stricmp(attrValue, "false") == 0) ||
                    (_stricmp(attrValue, "0") == 0) ||
                    (_stricmp(attrValue, "no") == 0) ||
                    (_stricmp(attrValue, "nope") == 0) ||
                    (_stricmp(attrValue, "none") == 0);

        if (isTrue || isFalse)
        {
            *v = isTrue;
            return true;
        }

        return false;
    }

    REIMPL void Hooked_LoadRuntimeValues(hta::ai::CinematicMover* self, void*, m3d::cmn::XmlFile* xmlFile, m3d::cmn::XmlNode* xmlNode)
    {
        self->Obj::LoadRuntimeValues(xmlFile, xmlNode);

        SafeStrAttrib(&self->m_flyPathName, xmlNode, "FlyPathName");

        if (!xmlNode->IsEmpty())
        {
            const char* timeStr = xmlNode->GetAttribute("CurrentFlyTime");
            if (timeStr) {
                self->m_currentFlyTime = (float)std::atof(timeStr);
            }
        }

        if (!xmlNode->IsEmpty())
        {
            const char* objIdStr = xmlNode->GetAttribute("ControlledObjId");
            if (objIdStr) {
                self->m_controlledObjId = std::atoi(objIdStr);
            }
        }

        MeridianState& state = GetState(self);

        if (!xmlNode->IsEmpty())
        {
            const char* modeStr = xmlNode->GetAttribute("MovingMode");
            if (modeStr) {
                state.movingMode = std::atoi(modeStr);
            }
        }

        SafeBoolAttrib(&state.oldObjectCinematicMode, xmlNode, "OldObjectCinematicMode");
        SafeBoolAttrib(&state.oldObjectGravityMode, xmlNode, "OldObjectGravityMode");

        m3d::cmn::XmlNode* childNode = xmlFile->CreateNode(m3d::cmn::XML_NODE_EMPTY, 0);

        if (childNode) {
            childNode->IncRef();
        } else {
            m3d::Kernel::Instance()->SysError("0 != m_ptr", "e:\\cruisecontrol\\work\\checkout\\truxx15\\trunk\\core\\ref_ptr.h");
        }

        xmlNode->GetFirstChild(childNode, "CurrentFlyPath");

        if (!childNode) {
            m3d::Kernel::Instance()->SysError("0 != m_ptr", "e:\\cruisecontrol\\work\\checkout\\truxx15\\trunk\\core\\ref_ptr.h");
        }

        if (!childNode->IsEmpty())
        {
            self->m_currentFlyPath = (m3d::CameraPath*)m3d::Kernel::Instance()->g_mar.AllocMem(sizeof(m3d::CameraPath), 0, 0);

            if (self->m_currentFlyPath)
            {
                auto* ctor = reinterpret_cast<void(__thiscall*)(m3d::CameraPath*)>(0x00629E00);
                ctor(self->m_currentFlyPath);
            }

            if (self->m_currentFlyPath) {
                self->m_currentFlyPath->LoadFromXmlRuntime(xmlFile, childNode);
            }
        }

        CinematicMover_AttachControlledObj(self);

        childNode->DecRef();
    }

    REIMPL void __fastcall Hooked_SaveRuntimeValues(const ai::CinematicMover* self, void*, m3d::cmn::XmlFile* xmlFile, m3d::cmn::XmlNode* xmlNode)
    {
        self->Obj::SaveRuntimeValues(xmlFile, xmlNode);

        xmlNode->SetAttribute("FlyPathName", self->m_flyPathName.m_charPtr);

        const MeridianState& state = GetState(self);

        xmlNode->SetAttribute("CurrentFlyTime",         CStr(self->m_currentFlyTime).c_str());
        xmlNode->SetAttribute("ControlledObjId",        CStr(self->m_controlledObjId).c_str());
        xmlNode->SetAttribute("MovingMode",             CStr(state.movingMode).c_str());
        xmlNode->SetAttribute("OldObjectCinematicMode", CStr(state.oldObjectCinematicMode).c_str());
        xmlNode->SetAttribute("OldObjectGravityMode",   CStr(state.oldObjectGravityMode).c_str());

        if (self->m_currentFlyPath) {
            m3d::cmn::XmlNode* flyPathNode = xmlFile->CreateNode(hta::m3d::cmn::XML_NODE_ELEMENT, "CurrentFlyPath");

            if (flyPathNode) {
                flyPathNode->IncRef();

                self->m_currentFlyPath->SaveToXmlRuntime(xmlFile, flyPathNode);

                xmlNode->AddChild(flyPathNode);

                flyPathNode->DecRef();
            }
            else {
                m3d::Kernel::Instance()->SysError("0 != m_ptr", "Failed to create XmlNode 'CurrentFlyPath'");
            }
        }
    }

    static void __fastcall Hooked_Update(hta::ai::CinematicMover* self, void*, float elapsedTime, uint32_t workTime) {
        if (!self || elapsedTime < 0.0f) {
            return;
        }

        self->Obj::Update(elapsedTime, workTime);

        MeridianState& state = GetState(self);
        if (state.movingMode == 0) {
            hta::ai::PhysicObj* controlledObj = self->_GetControlledObj();
            if (controlledObj && self->m_currentFlyPath) {
                SetCorrectPosToControlledObj(self, controlledObj, elapsedTime);

                self->m_currentFlyTime += elapsedTime;
            }
        }

        if (hta::ai::PhysicObj* controlledObj = self->_GetControlledObj()) {
            controlledObj->m_timeFromLastCollisionEffect += elapsedTime;
        }

        if (!self->m_currentFlyPath || self->m_currentFlyTime >= self->m_currentFlyPath->GetFullTime()) {
            self->Remove();
        }
    }

    REIMPL void __fastcall Hooked_SetObjAndPath(hta::ai::CinematicMover* self, void*, int32_t controlledObjId, const CStr& cinematicPathName, float totalTime)
    {
        if (self->m_currentFlyPath != nullptr)
        {
            m3d::CameraPathState* myFirst = self->m_currentFlyPath->m_cameraPathStates._Myfirst;
            if (myFirst != nullptr) {
                m3d::Kernel::Instance()->g_mar.FreeMem(myFirst, 0, 0);
            }

            self->m_currentFlyPath->m_cameraPathStates._Myfirst = nullptr;
            self->m_currentFlyPath->m_cameraPathStates._Mylast = nullptr;
            self->m_currentFlyPath->m_cameraPathStates._Myend = nullptr;

            m3d::Kernel::Instance()->g_mar.FreeMem(self->m_currentFlyPath, 0, 0);
            self->m_currentFlyPath = nullptr;
        }

        const hta::ai::CServer* server = hta::ai::CServer::Instance();

        if (self->m_controlledObjId >= 0)
        {
            ai::Obj* oldObj = server->m_pObjects->GetEntityByObjId(self->m_controlledObjId);
            if (oldObj != nullptr) {
                _DetachControlledObj(self);
            }
        }

        self->m_currentFlyTime = 0.0f;
        self->m_controlledObjId = controlledObjId;
        self->m_flyPathName = cinematicPathName;

        if (self->m_controlledObjId >= 0) {
            ai::PhysicObj* controlledObj = (ai::PhysicObj*)server->m_pObjects->GetEntityByObjId(self->m_controlledObjId);

            if (controlledObj != nullptr)
            {
                int existingMoverId = PhysicObj_GetCinematicMoverId(controlledObj);
                if (existingMoverId >= 0)
                {
                    ai::CinematicMover* existingMover = (ai::CinematicMover*)server->m_pObjects->GetEntityByObjId(existingMoverId);
                    if (existingMover != nullptr) {
                        _DetachControlledObj(existingMover);
                    }
                }

                MeridianState& state = GetState(self);
                state.movingMode = PhysicObj_IsPhysicsEnabled(controlledObj);
                CinematicMover_AttachControlledObj(self);
                PhysicObj_SetCinematicMoverId(controlledObj, self->m_objId);

                m3d::Cinematic* cinematic = GetCinematic();
                cinematic->Load("camera_paths.xml");

                self->m_currentFlyPath = (m3d::CameraPath*)m3d::Kernel::Instance()->g_mar.AllocMem(sizeof(m3d::CameraPath), 0, 0);

                if (self->m_currentFlyPath != nullptr)
                {
                    const m3d::CameraPath& pathByName = cinematic->GetPathByName(self->m_flyPathName);

                    auto* currentPath = static_cast<hta::m3d::CameraPath*>(m3d::Kernel::Instance()->g_mar.AllocMem(static_cast<int32_t>(sizeof(hta::m3d::CameraPath)), nullptr, 0));
                    if (!currentPath) {
                        return;
                    }

                    auto* copyCtor = reinterpret_cast<void(__thiscall*)(hta::m3d::CameraPath*, const hta::m3d::CameraPath&)>(0x0062A220);
                    copyCtor(currentPath, pathByName);
                    self->m_currentFlyPath = currentPath;

                    self->m_currentFlyPath->m_fullLength = pathByName.m_fullLength;
                    self->m_currentFlyPath->m_fullTime = pathByName.m_fullTime;

                    m3d::CameraPathState initialState;
                    initialState.m_point = controlledObj->GetPosition();
                    initialState.m_rotation = controlledObj->GetRotation();
                    initialState.m_zoom = 1.0f;
                    initialState.m_speed = 1.0f;

                    self->m_currentFlyPath->insert(0, initialState);

                    self->m_currentFlyPath->m_fullTime = totalTime;
                    self->m_currentFlyPath->CalcFlyTimes(1, 1);
                }
            }
        }
    }

    void Apply() {
        LOG_INFO("Feature enabled");

        kraken::routines::Redirect(34, (void*)0x00692880, (void*)&Hooked_ObjRemove);
        kraken::routines::Redirect(280, (void*)0x007FA4E0, (void*)&Hooked_SaveRuntimeValues);
        kraken::routines::Redirect(230, (void*)0x007FA670, (void*)&Hooked_Update);
        kraken::routines::Redirect(306, (void*)0x007FA760, (void*)&Hooked_LoadRuntimeValues);
        kraken::routines::Redirect(833, (void*)0x007FA950, (void*)&Hooked_SetObjAndPath);
        kraken::routines::Redirect(0xC0, (void*)0x00890DD0, (void*)&Hooked_CollidePOAndWater);

        LOG_INFO("CinematicMover Meridian port applied");
    }
}
