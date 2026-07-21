#define LOGGER "bossmetalarm"

#include <cmath>
#include <cstdint>

#include "ext/logger.hpp"
#include "routines.hpp"
#include "config.hpp"

// ai::BossMetalArm ("boss01", кран-манипулятор r1m4).
namespace kraken::fix::bossmetalarm {
    struct Vec3 { float x, y, z; };

    using UpdateFn              = void(__thiscall*)(void* self, float elapsedTime, unsigned int workTime);
    using SetAttackStateFn      = void(__thiscall*)(void* self, int newState);
    using GetPositionFn         = Vec3*(__thiscall*)(void* self, Vec3* out);
    using SetLinearVelocityFn   = void(__thiscall*)(void* self, Vec3* vel);
    using GetVehicleByPlayerFn  = void*(__thiscall*)(void* dynamicScene);
    using GetEntityByObjIdFn    = void*(__thiscall*)(void* objContainer, int objId);

    static const auto OriginalUpdate             = reinterpret_cast<UpdateFn>(0x00733E20);            // ai::BossMetalArm::Update
    static const auto SetAttackState             = reinterpret_cast<SetAttackStateFn>(0x00732580);    // ai::BossMetalArm::_SetAttackState
    static const auto GetPosition                = reinterpret_cast<GetPositionFn>(0x005FC410);       // ai::PhysicObj::GetPosition
    static const auto SetLinearVelocity          = reinterpret_cast<SetLinearVelocityFn>(0x005FA730); // ai::PhysicObj::SetLinearVelocity
    static const auto GetVehicleControlledByPlayer = reinterpret_cast<GetVehicleByPlayerFn>(0x006013A0); // ai::DynamicScene::GetVehicleControlledByPlayer
    static const auto GetEntityByObjId           = reinterpret_cast<GetEntityByObjIdFn>(0x0040C310);  // ai::ObjContainer::GetEntityByObjId

    constexpr uintptr_t PTR_DYNAMIC_SCENE  = 0x00A12958; // глобальный указатель на ai::DynamicScene
    constexpr uintptr_t PTR_OBJ_CONTAINER  = 0x00A12E98; // глобальный указатель на ai::ObjContainer
    constexpr uintptr_t PTR_THROW_SPEED    = 0x009E5A20; // родная константа горизонтальной скорости броска (50.0)

    constexpr int OFF_LOAD_OBJ_ID  = 0x144; // ai::BossMetalArm::m_loadObjId
    constexpr int OFF_ATTACK_STATE = 0x14C; // ai::BossMetalArm::m_attackState
    constexpr int STATE_CHARGING   = 2;     // "Boss is charging"
    constexpr int STATE_ATTACKING  = 3;     // "Boss is attacking"

    // Ванильный бросок целится прямой линией на игрока с фиксированной скоростью и не
    // учитывает гравитацию — на большой дистанции бочка не долетает, падая раньше цели.
    // Пересчитываем скорость так, чтобы бочка гарантированно попадала в текущую позицию
    // игрока за время полёта, определяемое исходной горизонтальной скоростью.
    void FixThrowTrajectory(int loadObjId) {
        void* objContainer = *reinterpret_cast<void**>(PTR_OBJ_CONTAINER);
        if (!objContainer)
            return;

        void* loadObj = GetEntityByObjId(objContainer, loadObjId);
        if (!loadObj)
            return;

        void* dynamicScene = *reinterpret_cast<void**>(PTR_DYNAMIC_SCENE);
        if (!dynamicScene)
            return;

        void* playerVehicle = GetVehicleControlledByPlayer(dynamicScene);
        if (!playerVehicle)
            return;

        Vec3 launchPos{};
        Vec3 targetPos{};
        GetPosition(loadObj, &launchPos);
        GetPosition(playerVehicle, &targetPos);

        float dx = targetPos.x - launchPos.x;
        float dy = targetPos.y - launchPos.y;
        float dz = targetPos.z - launchPos.z;

        float horizDist = std::sqrt(dx * dx + dz * dz);
        float baseSpeed = *reinterpret_cast<float*>(PTR_THROW_SPEED);

        float flightTime = horizDist / baseSpeed;
        if (flightTime < 0.35f)
            flightTime = 0.35f; // защита от деления на ~0 при бросках в упор

        float gravity = -kraken::Config::Instance().gravity.value; // конфиг хранит отрицательное ускорение (по умолчанию -9.81)

        Vec3 vel;
        vel.x = dx / flightTime;
        vel.z = dz / flightTime;
        vel.y = (dy + 0.5f * gravity * flightTime * flightTime) / flightTime;

        SetLinearVelocity(loadObj, &vel);
    }

    void __fastcall UpdateHook(void* self, void* /*edx*/, float elapsedTime, unsigned int workTime) {
        char* raw = reinterpret_cast<char*>(self);
        int attackState   = *reinterpret_cast<int*>(raw + OFF_ATTACK_STATE);
        int prevLoadObjId = *reinterpret_cast<int*>(raw + OFF_LOAD_OBJ_ID);

        // Ванильный код держит "Boss is charging" до конца родной анимации замаха
        // (GetNodeRealAnimAction() == 1), даже когда бочка уже в клешне и ждать
        // больше нечего. Как только груз создан — сразу пускаем в "Attacking".
        if (attackState == STATE_CHARGING && prevLoadObjId != -1) {
            SetAttackState(self, STATE_ATTACKING);
        }

        OriginalUpdate(self, elapsedTime, workTime);

        int newLoadObjId = *reinterpret_cast<int*>(raw + OFF_LOAD_OBJ_ID);
        if (prevLoadObjId != -1 && newLoadObjId == -1) {
            // Груз только что отпущен этим тиком (движок сбрасывает m_loadObjId в -1
            // сразу после броска) — родная скорость уже выставлена неверно, правим её.
            FixThrowTrajectory(prevLoadObjId);
        }
    }

    void Apply() {
        const kraken::Config& config = kraken::Config::Instance();
        if (config.boss01_fast_charge.value) {
            LOG_INFO("Feature enabled");
            kraken::routines::OverrideValue(reinterpret_cast<void*>(0x009A37F0), (void*)&UpdateHook); // ai::BossMetalArm vftable slot for Update
        }
    }
}
