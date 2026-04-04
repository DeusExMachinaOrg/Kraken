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


namespace kraken::fix::cinematicmover {
    namespace {
        struct MeridianState {
            int32_t movingMode = 0;
            bool oldObjectCinematicMode = false;
            bool oldObjectGravityMode = false;
            bool oldObjectAutoDisableFlag = false;
        };

        static void SetCorrectPosToControlledObj(hta::ai::CinematicMover* self, hta::ai::PhysicObj* controlledObj);
        static void __fastcall ControlledObjBeforeStepCallback(dxBody* body);

        static float GetPhysicsStepTime() {
            const auto* globalProps = hta::ai::GlobalProperties::Instance();
            if (!globalProps) {
                return 1.0f / 60.0f;
            }

            const float dt = globalProps->m_physicStepTime;
            return (dt > 0.0f && dt < 1.0f) ? dt : (1.0f / 60.0f);
        }

        using PhysicObjVoidIntFn = void(__thiscall*)(hta::ai::PhysicObj*, int32_t);
        using PhysicObjGetIntConstFn = int32_t(__thiscall*)(const hta::ai::PhysicObj*);

        static std::unordered_map<const hta::ai::PhysicObj*, int32_t> g_cinematicMoverIds;
        static std::unordered_map<const dxBody*, void*> g_beforeStepCallbacks;
        static std::unordered_map<const dxBody*, void*> g_savedBeforeStepCallbacks;

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

            reinterpret_cast<void(__fastcall*)(dxBody*, bool)>(0x007C4BF0)(rawBody, value);
        }

        static void PhysicObj_EnableBody(hta::ai::PhysicObj* obj) {
            if (!obj) {
                return;
            }

            dxBody* rawBody = obj->GetBody() ? obj->GetBody()->_id : nullptr;
            if (!rawBody) {
                return;
            }

            reinterpret_cast<void(__fastcall*)(dxBody*)>(0x007C67D0)(rawBody);
        }

        static bool PhysicObj_GetAutoDisableFlag(const hta::ai::PhysicObj* obj) {
            if (!obj || !obj->GetBody()) {
                return false;
            }

            const dxBody* rawBody = obj->GetBody()->_id;
            // dBodyGetAutoDisableFlag: (flags >> 4) & 1
            return rawBody && ((static_cast<uint32_t>(rawBody->flags) >> 4) & 1u) != 0;
        }

        static void PhysicObj_SetAutoDisableFlag(hta::ai::PhysicObj* obj, bool value) {
            if (!obj || !obj->GetBody()) {
                return;
            }

            dxBody* rawBody = obj->GetBody()->_id;
            if (!rawBody) {
                return;
            }

            reinterpret_cast<void(__fastcall*)(dxBody*, bool)>(0x007C4CB0)(rawBody, value);
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

        static void CinematicMover_AttachControlledObj(hta::ai::CinematicMover* self, hta::ai::PhysicObj* controlledObj) {
            if (!self || !controlledObj) {
                return;
            }

            MeridianState& state = GetState(self);
            if (state.movingMode != 1) {
                return;
            }

            state.oldObjectCinematicMode = PhysicObj_IsCinematic(controlledObj);
            state.oldObjectGravityMode = PhysicObj_GetGravityMode(controlledObj);
            state.oldObjectAutoDisableFlag = PhysicObj_GetAutoDisableFlag(controlledObj);

            PhysicObj_SetCinematic(controlledObj, true);
            PhysicObj_SetGravityMode(controlledObj, false);
            PhysicObj_SetAutoDisableFlag(controlledObj, false);

            if (controlledObj->GetBody()) {
                dxBody* rawBody = controlledObj->GetBody()->_id;
                g_savedBeforeStepCallbacks[rawBody] = dBody_GetBeforeStepCallback(rawBody);
                dBody_SetBeforeStepCallback(rawBody, reinterpret_cast<void*>(&ControlledObjBeforeStepCallback));
            }

            if (PhysicObj_IsCinematic(controlledObj)) {
                const hta::CVector zero(0.0f, 0.0f, 0.0f);
                controlledObj->SetLinearVelocity(zero);
                controlledObj->SetAngularVelocity(zero);
            }
        }

        static void DetachControlledObj(hta::ai::CinematicMover* self, hta::ai::PhysicObj* controlledObj) {
            if (!self || !controlledObj) {
                return;
            }

            MeridianState& state = GetState(self);
            if (state.movingMode != 1) {
                return;
            }

            PhysicObj_SetCinematicMoverId(controlledObj, -1);

            if (controlledObj->GetBody()) {
                dxBody* rawBody = controlledObj->GetBody()->_id;
                const auto it = g_savedBeforeStepCallbacks.find(rawBody);
                dBody_SetBeforeStepCallback(rawBody, it != g_savedBeforeStepCallbacks.end() ? it->second : nullptr);
                g_savedBeforeStepCallbacks.erase(rawBody);
            }

            if (PhysicObj_IsCinematic(controlledObj)) {
                const hta::CVector zero(0.0f, 0.0f, 0.0f);
                controlledObj->SetLinearVelocity(zero);
                controlledObj->SetAngularVelocity(zero);
            }

            PhysicObj_SetCinematic(controlledObj, state.oldObjectCinematicMode);
            PhysicObj_SetGravityMode(controlledObj, state.oldObjectGravityMode);
            PhysicObj_SetAutoDisableFlag(controlledObj, state.oldObjectAutoDisableFlag);
        }

        static void __fastcall ControlledObjBeforeStepCallback(dxBody* body) {
            static uint32_t callbackHitCounter = 0;

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

            const float callbackStep = GetPhysicsStepTime();
            SetCorrectPosToControlledObj(mover, controlledObj);
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

        static void SetCorrectPosToControlledObj(hta::ai::CinematicMover* self, hta::ai::PhysicObj* controlledObj) {
            if (!self || !controlledObj || !self->m_currentFlyPath) {
                return;
            }

            const float sampleStep = GetPhysicsStepTime();

            hta::CVector currentPos;
            hta::Quaternion currentRot;
            float currentZoom = 0.0f;
            if (!SamplePath(self->m_currentFlyPath, self->m_currentFlyTime, currentPos, currentRot, currentZoom)) {
                return;
            }

            controlledObj->SetPosition(currentPos);
            controlledObj->SetRotation(currentRot);

            hta::CVector linearVelocity(0.0f, 0.0f, 0.0f);
            hta::CVector angularVelocity(0.0f, 0.0f, 0.0f);
            if (sampleStep > 0.0f && self->m_currentFlyTime > sampleStep) {
                hta::CVector prevPos;
                hta::Quaternion prevRot;
                float prevZoom = 0.0f;
                if (SamplePath(self->m_currentFlyPath, self->m_currentFlyTime - sampleStep, prevPos, prevRot, prevZoom)) {
                    linearVelocity = (currentPos - prevPos) / sampleStep;

                    auto quaternionToMatrix3x3 = [](const hta::Quaternion& q, float m[3][3]) {
                        float x = q.x;
                        float y = q.y;
                        float z = q.z;
                        float w = q.w;
                        const float norm = std::sqrt(x * x + y * y + z * z + w * w);
                        if (norm > 1e-6f) {
                            const float invNorm = 1.0f / norm;
                            x *= invNorm;
                            y *= invNorm;
                            z *= invNorm;
                            w *= invNorm;
                        }

                        const float xx = x * x;
                        const float yy = y * y;
                        const float zz = z * z;
                        const float xy = x * y;
                        const float xz = x * z;
                        const float yz = y * z;
                        const float wx = w * x;
                        const float wy = w * y;
                        const float wz = w * z;

                        m[0][0] = 1.0f - 2.0f * (yy + zz);
                        m[0][1] = 2.0f * (xy - wz);
                        m[0][2] = 2.0f * (xz + wy);
                        m[1][0] = 2.0f * (xy + wz);
                        m[1][1] = 1.0f - 2.0f * (xx + zz);
                        m[1][2] = 2.0f * (yz - wx);
                        m[2][0] = 2.0f * (xz - wy);
                        m[2][1] = 2.0f * (yz + wx);
                        m[2][2] = 1.0f - 2.0f * (xx + yy);
                    };

                    float prevM[3][3];
                    float currM[3][3];
                    quaternionToMatrix3x3(prevRot, prevM);
                    quaternionToMatrix3x3(currentRot, currM);

                    float dR[3][3] = {};
                    for (int r = 0; r < 3; ++r) {
                        for (int c = 0; c < 3; ++c) {
                            dR[r][c] = (currM[r][c] - prevM[r][c]) / sampleStep;
                        }
                    }

                    float omegaSkew[3][3] = {};
                    for (int r = 0; r < 3; ++r) {
                        for (int c = 0; c < 3; ++c) {
                            omegaSkew[r][c] =
                                dR[r][0] * currM[c][0] +
                                dR[r][1] * currM[c][1] +
                                dR[r][2] * currM[c][2];
                        }
                    }

                    angularVelocity.x = 0.5f * (omegaSkew[2][1] - omegaSkew[1][2]);
                    angularVelocity.y = 0.5f * (omegaSkew[0][2] - omegaSkew[2][0]);
                    angularVelocity.z = 0.5f * (omegaSkew[1][0] - omegaSkew[0][1]);
                }
            }

            controlledObj->SetLinearVelocity(linearVelocity);
            controlledObj->SetAngularVelocity(angularVelocity);
            (void)currentZoom;
        }
    }

    void OnBodyBeforeStep(void* body) {
        if (!body) {
            return;
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
                DetachControlledObj(mover, controlledObj);
            }
            g_states.erase(mover);
        }

        // Full replacement of ai::Obj::Remove (HTA RVA 0x292880, 34 bytes).
        self->_SetDeadStatus();
        self->m_flags |= 2u;
        hta::ai::CServer* server = hta::ai::CServer::Instance();
        hta::ai::ObjContainer* objcont = server ? server->m_pObjects : nullptr;
        if (objcont && self->m_objId != -1) {
            objcont->AddObjIdToRemove(self->m_objId);
        }
    }

    static void __fastcall Hooked_LoadRuntimeValues(hta::ai::CinematicMover* self, void*, hta::m3d::cmn::XmlFile* xmlFile, const hta::m3d::cmn::XmlNode* xmlNode) {
        if (!self || !xmlFile || !xmlNode) {
            return;
        }

        self->Obj::LoadRuntimeValues(xmlFile, xmlNode);

        if (const char* flyPath = xmlNode->GetAttribute("FlyPathName"); flyPath && *flyPath) {
            self->m_flyPathName = flyPath;
        }

        int32_t controlledObjId = self->m_controlledObjId;
        if (TryParseIntAttribute(xmlNode, "ControlledObjId", controlledObjId)) {
            self->m_controlledObjId = controlledObjId;
        }

        const char* currentFlyTimeText = xmlNode->GetAttribute("CurrentFlyTime");
        if (currentFlyTimeText && *currentFlyTimeText) {
            self->m_currentFlyTime = static_cast<float>(std::atof(currentFlyTimeText));
        }

        MeridianState& state = GetState(self);
        TryParseIntAttribute(xmlNode, "MovingMode", state.movingMode);
        TryParseBoolAttribute(xmlNode, "OldObjectCinematicMode", state.oldObjectCinematicMode);
        TryParseBoolAttribute(xmlNode, "OldObjectGravityMode", state.oldObjectGravityMode);
        TryParseBoolAttribute(xmlNode, "OldObjectAutoDisableFlag", state.oldObjectAutoDisableFlag);

        auto* runtimePathNode = xmlFile->CreateNode(hta::m3d::cmn::XML_NODE_EMPTY, nullptr);
        if (runtimePathNode) {
            if (xmlNode->GetFirstChild(runtimePathNode, "CurrentFlyPath") && !runtimePathNode->IsEmpty()) {
                if (hta::m3d::CameraPath* currentPath = EnsureCurrentFlyPath(self)) {
                    currentPath->LoadFromXmlRuntime(xmlFile, runtimePathNode);
                }
            }
            runtimePathNode->DecRef();
        }

        hta::ai::PhysicObj* controlledObj = self->_GetControlledObj();
        if (!controlledObj) {
            return;
        }

        CinematicMover_AttachControlledObj(self, controlledObj);
    }

    static void __fastcall Hooked_SaveRuntimeValues(const hta::ai::CinematicMover* self, void*, hta::m3d::cmn::XmlFile* xmlFile, hta::m3d::cmn::XmlNode* xmlNode) {
        if (!self || !xmlFile || !xmlNode) {
            return;
        }

        self->Obj::SaveRuntimeValues(xmlFile, xmlNode);

        xmlNode->SetAttribute("FlyPathName", self->m_flyPathName.c_str());
        WriteIntAttribute(xmlNode, "ControlledObjId", self->m_controlledObjId);
        char flyTimeText[32] = {};
        std::snprintf(flyTimeText, sizeof(flyTimeText), "%.9g", self->m_currentFlyTime);
        xmlNode->SetAttribute("CurrentFlyTime", flyTimeText);

        const MeridianState& state = GetState(self);
        WriteIntAttribute(xmlNode, "MovingMode", state.movingMode);
        WriteBoolAttribute(xmlNode, "OldObjectCinematicMode", state.oldObjectCinematicMode);
        WriteBoolAttribute(xmlNode, "OldObjectGravityMode", state.oldObjectGravityMode);
        WriteBoolAttribute(xmlNode, "OldObjectAutoDisableFlag", state.oldObjectAutoDisableFlag);

        if (self->m_currentFlyPath) {
            auto* runtimePathNode = xmlFile->CreateNode(hta::m3d::cmn::XML_NODE_ELEMENT, "CurrentFlyPath");
            if (runtimePathNode) {
                self->m_currentFlyPath->SaveToXmlRuntime(xmlFile, runtimePathNode);
                xmlNode->AddChild(runtimePathNode);
                runtimePathNode->DecRef();
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
                SetCorrectPosToControlledObj(self, controlledObj);

                self->m_currentFlyTime += elapsedTime;
            }
        }

        if (!self->m_currentFlyPath || self->m_currentFlyTime >= self->m_currentFlyPath->GetFullTime()) {
            self->Remove();
        }
    }

    static void __fastcall Hooked_SetObjAndPath(hta::ai::CinematicMover* self, void*, int32_t controlledObjId, const hta::CStr& flyPathName, float flyTime) {
        if (!self) {
            return;
        }

        ReleaseCurrentFlyPath(self);

        hta::ai::PhysicObj* oldControlledObj = self->_GetControlledObj();
        if (oldControlledObj) {
            DetachControlledObj(self, oldControlledObj);
        }

        self->m_controlledObjId = controlledObjId;
        self->m_flyPathName = flyPathName;
        self->m_currentFlyTime = 0.0f;

        MeridianState& state = GetState(self);
        state.movingMode = 0;

        hta::ai::PhysicObj* controlledObj = self->_GetControlledObj();
        if (!controlledObj) {
            return;
        }

        const int32_t oldMoverId = PhysicObj_GetCinematicMoverId(controlledObj);
        if (oldMoverId >= 0) {
            const hta::ai::CServer* server = hta::ai::CServer::Instance();
            if (server && server->m_pObjects) {
                hta::ai::Obj* oldMoverObj = server->m_pObjects->GetEntityByObjId(oldMoverId);
                if (oldMoverObj && oldMoverObj->GetClass()->IsKindOf(hta::ai::CinematicMover::GetBaseClass())) {
                    auto* oldMover = static_cast<hta::ai::CinematicMover*>(oldMoverObj);
                    DetachControlledObj(oldMover, controlledObj);
                }
            }
        }

        PhysicObj_EnableBody(controlledObj);
        state.movingMode = PhysicObj_IsPhysicsEnabled(controlledObj) ? 1 : 0;
        CinematicMover_AttachControlledObj(self, controlledObj);
        PhysicObj_SetCinematicMoverId(controlledObj, self->m_objId);

        hta::m3d::Cinematic* cinematic = GetCinematic();
        if (!cinematic || self->m_flyPathName.empty()) {
            return;
        }

        cinematic->Load("camera_paths.xml");
        const hta::m3d::CameraPath& sourcePath = cinematic->GetPathByName(self->m_flyPathName);

        hta::m3d::Kernel* kernel = hta::m3d::Kernel::Instance();
        if (!kernel || !kernel->m_memMan) {
            return;
        }

        auto* currentPath = static_cast<hta::m3d::CameraPath*>(kernel->m_memMan->Malloc(static_cast<int32_t>(sizeof(hta::m3d::CameraPath)), nullptr, 0));
        if (!currentPath) {
            return;
        }

        auto* copyCtor = reinterpret_cast<void(__thiscall*)(hta::m3d::CameraPath*, const hta::m3d::CameraPath&)>(0x0062A220);
        copyCtor(currentPath, sourcePath);
        self->m_currentFlyPath = currentPath;

        hta::m3d::CameraPathState initialState(controlledObj->GetPosition(), controlledObj->GetRotation(), 0.0f, 0.0f, 0.0f);
        currentPath->insert(0, initialState);
        currentPath->SetFullTime(flyTime);
        currentPath->CalcFlyTimes(1, true);
    }

    void Apply() {
        LOG_INFO("Feature enabled");

        kraken::routines::Redirect(34, (void*)0x00692880, (void*)&Hooked_ObjRemove);
        kraken::routines::Redirect(280, (void*)0x007FA4E0, (void*)&Hooked_SaveRuntimeValues);
        kraken::routines::Redirect(230, (void*)0x007FA670, (void*)&Hooked_Update);
        kraken::routines::Redirect(306, (void*)0x007FA760, (void*)&Hooked_LoadRuntimeValues);
        kraken::routines::Redirect(833, (void*)0x007FA950, (void*)&Hooked_SetObjAndPath);

        LOG_INFO("CinematicMover Meridian port applied");
    }
}
