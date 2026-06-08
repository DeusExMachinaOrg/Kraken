#include "hta/CStr.hpp"
#include "hta/CVector.hpp"
#include "hta/ai/Gun.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/ai/PhysicObj.hpp"
#include "hta/ai/DamageInfo.hpp"
#include "hta/ai/Enums.hpp"
#include "hta/ai/DynamicScene.hpp"
#include "ext/ai/Appendix.hpp"
#include "routines.hpp"

#include <unordered_map>

using namespace hta;

namespace hta::ai {
    struct DynamicScene;
    extern DynamicScene* gDynamicScene;

    static inline float __cdecl CalcHitValue(float speed) {
        return reinterpret_cast<float(__cdecl*)(float)>(0x008C28F0)(speed);
    }

    static inline float __cdecl CalcSideCoeff(float dot) {
        return reinterpret_cast<float(__cdecl*)(float)>(0x008C26B0)(dot);
    }
}

namespace {
    static auto dBodyGetPointVelPtr = reinterpret_cast<void(__fastcall*)(dxBody*, float, float, float, float*)>(0x007C49E0);
    static std::unordered_map<const m3d::Class*, float> g_vehiclePrototypeHitForceByType;

    static float GetPrototypeHitForce(const ai::VehiclePrototypeInfo* prototype) {
        if (!prototype) {
            return 10.0f;
        }

        const m3d::Class* prototypeClass = prototype->m_protoClassObject;
        if (!prototypeClass) {
            return 10.0f;
        }

        auto it = g_vehiclePrototypeHitForceByType.find(prototypeClass);
        return it != g_vehiclePrototypeHitForceByType.end() ? it->second : 10.0f;
    }

    static CVector NormalizeSafe(const CVector& vec) {
        CVector result = vec;
        if (result.LengthSquare() > 1e-8f) {
            result.Normalize();
        }
        return result;
    }
}

namespace kraken::fix::thorncollide {
    double __fastcall CalcDamageToVehicles(
        ai::Vehicle *v1,
        ai::Vehicle *v2,
        dContact *contacts,
        float *dSpeed,
        ai::DamageInfo *damageInfo)
    {
        const bool isVehicle1 = v1 ? v1->cast<ai::Vehicle>() : false;
        const bool isVehicle2 = v2 ? v2->cast<ai::Vehicle>() : false;

        CVector tempVec; // Временный вектор, куда движок пишет результат при вызове GetPosition/GetDirection

        // 1. Извлекаем позицию контакта
        CVector contactPos(contacts->geom.pos[0], contacts->geom.pos[1], contacts->geom.pos[2]);

        // 2. Вектор направления от Машины 1 к точке контакта
        CVector pos1 = v1->GetPosition();
        CVector dirToContact1 = NormalizeSafe(contactPos - pos1);

        // 3. Вычисляем относительную скорость в точке контакта (через ODE)
        float velOde[4] = {0};
        dBodyGetPointVelPtr(v1->m_body->_id, contactPos.x, contactPos.y, contactPos.z, velOde);
        CVector pointVel1(velOde[0], velOde[1], velOde[2]);
        CVector pointVel2(0.0f, 0.0f, 0.0f);

        if (v2) {
            dBodyGetPointVelPtr(v2->m_body->_id, contactPos.x, contactPos.y, contactPos.z, velOde);
            pointVel2 = CVector(velOde[0], velOde[1], velOde[2]);
        }

        CVector deltaVel = pointVel1 - pointVel2;

        // 4. Обрабатываем нормаль контакта
        CVector contactNormal(contacts->geom.normal[0], contacts->geom.normal[1], contacts->geom.normal[2]);
        contactNormal = NormalizeSafe(contactNormal);

        // Если нормаль контакта "смотрит" против направления удара, инвертируем её
        if (CVector::Dot(contactNormal, dirToContact1) < 0.0f) {
            contactNormal = contactNormal * -1.0f;
        }

        // 5. Вычисляем скорость удара (проекция относительной скорости на нормаль)
        *dSpeed = fabs(CVector::Dot(contactNormal, deltaVel));
        float hitValue = ai::CalcHitValue(*dSpeed);

        // 6. Вычисляем множители стороны удара (лоб, бок, зад)
        CVector v1Dir = v1->GetDirection();
        float side1 = ai::CalcSideCoeff(CVector::Dot(v1Dir, dirToContact1));
        float side2 = 1.0f;

        if (isVehicle2 && v2) {
            CVector pos2 = v2->GetPosition();
            CVector dirToContact2 = NormalizeSafe(contactPos - pos2);

            CVector v2Dir = v2->GetDirection();
            side2 = ai::CalcSideCoeff(CVector::Dot(v2Dir, dirToContact2));
        }

        // 7. Сила шипов/бампера (Thorn Force)
        float thorn1 = isVehicle1 ? kraken::ext::ai::GetVehicleThornForce(v1) : 1.0f;
        float thorn2 = isVehicle2 ? kraken::ext::ai::GetVehicleThornForce(v2) : 1.0f;

        // 8. Базовый урон машин из конфигурации (Prototype)
        const ai::VehiclePrototypeInfo* proto1 = v1->GetPrototypeInfo();
        const ai::VehiclePrototypeInfo* proto2 = v2 ? v2->GetPrototypeInfo() : proto1;

        float v1HitForce = GetPrototypeHitForce(proto1);
        float v2HitForce = GetPrototypeHitForce(proto2);

        // 9. Итоговый расчет асимметричного урона
        // Урон, который наносится второй машине (возвращается из функции)
        double damageToV2 = (v1HitForce / side2) * thorn1 * side1 * hitValue;

        // Урон, который получает первая машина (записывается в damageInfo)
        float damageToV1 = (v2HitForce / side1) * thorn2 * side2 * hitValue;

        // 10. Заполняем структуру отчета об уроне (DamageInfo)
        damageInfo->hitPos = contactPos;
        damageInfo->hitDir = contactNormal;
        damageInfo->normal = contactNormal;
        damageInfo->damage = damageToV1;
        damageInfo->damageType = hta::ai::DAMAGE_BLAST;

        // 11. Логика появления следов от удара
        if (*dSpeed <= 20.0f) {
            damageInfo->decalId = -1;
        } else {
            damageInfo->decalId = ai::DynamicScene::Instance()->m_clashDecalId;
        }

        return damageToV2;
    }

    int __fastcall CollideVehiclePartAndVehiclePart(
        ai::VehiclePart *obj1,
        ai::VehiclePart *obj2,
        dContact *contacts,
        unsigned int *numContacts,
        bool reverse)
    {
        // 1. Получаем владельцев частей (PhysicObj)
        ai::PhysicObj *owner1 = obj1->GetOwner();
        ai::PhysicObj *owner2 = obj2->GetOwner();

        // Если у кого-то из объектов нет владельца, выходим
        if (!owner1 || !owner2) {
            return 1;
        }

        // 2. Проверяем, являются ли владельцы машинами
        bool isVehicle1 = owner1->cast<ai::Vehicle>();
        bool isVehicle2 = owner2->cast<ai::Vehicle>();

        // Если оба объекта не машины (например, столкнулись два ящика), эта функция им не нужна
        if (!isVehicle1 && !isVehicle2) {
            return 1;
        }

        // 4. Нормализация порядка: гарантируем, что 'owner1' всегда будет машиной
        ai::VehiclePart *part1 = obj1;
        ai::VehiclePart *part2 = obj2;

        if (isVehicle2 && !isVehicle1) {
            std::swap(part1, part2);
            std::swap(owner1, owner2);
            isVehicle1 = true;
            isVehicle2 = false;
        }

        // 5. Вычисляем урон через функцию CalcDamageToVehicles
        float deltaSpeed = 0.0f;
        ai::DamageInfo damageInfo;

        float damageToVehicle2 = CalcDamageToVehicles(
            (ai::Vehicle*)owner1,
            isVehicle2 ? (ai::Vehicle*)owner2 : nullptr,
            contacts,
            &deltaSpeed,
            &damageInfo
        );

        // 6. Наносим урон первой машине
        if (isVehicle1) {
            damageInfo.damagedPartName = part1->m_partName;

            if (isVehicle2) {
                damageInfo.attackerId = owner2->m_objId;
                damageInfo.attackingAgentId = owner2->m_objId;
            }
            owner1->InflictDamage(damageInfo);
        }

        // 7. Наносим урон второй машине (если она существует)
        if (isVehicle2) {
            damageInfo.damagedPartName = part2->m_partName;

            // Для второй машины векторы удара должны быть направлены в противоположную сторону
            damageInfo.hitDir = damageInfo.hitDir * -1.0f;
            damageInfo.normal = damageInfo.normal * -1.0f;
            damageInfo.damage = damageToVehicle2;

            // После "нормализации порядка" (шаг 4) isVehicle1 всегда true здесь,
            // поэтому attackerId всегда будет первой машиной.
            damageInfo.attackerId = owner1->m_objId;
            damageInfo.attackingAgentId = owner1->m_objId;

            owner2->InflictDamage(damageInfo);
        }

        // 8. Визуальные эффекты (искры)
        if (owner1->CanCreateCollisionEffect() && owner2->CanCreateCollisionEffect()) {
            if (deltaSpeed > 1.0f) {

                // Помечаем, что эффект столкновения создан (чтобы не спамить в одном кадре)
                owner1->SetCollisionEffectCreated();
                owner2->SetCollisionEffectCreated();

                CStr effectName;
                if (deltaSpeed <= 10.0f) {
                    effectName = "ET_PS_VEHICLESPARKLE_SLOW";
                } else {
                    effectName = "ET_PS_VEHICLESPARKLE";
                }

                Quaternion rotation;
                rotation.Identity();

                CVector hitPos(contacts->geom.pos[0], contacts->geom.pos[1], contacts->geom.pos[2]);
                ai::PhysicBody::CreateEffectNode(effectName, hitPos, rotation, 1, 1.0f);
            }
        }

        return 1;
    }

    void Apply() {
        kraken::routines::Redirect(0x383, (void*)0x0088F700, (void*)&CalcDamageToVehicles);
        kraken::routines::Redirect(0x34A, (void*)0x00890430, (void*)&CollideVehiclePartAndVehiclePart);
    }
}