#include "hta/CStr.hpp"
#include "hta/CVector.hpp"
#include "hta/ai/Gun.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/ai/PhysicObj.hpp"
#include "hta/ai/DamageInfo.hpp"
#include "hta/ai/Enums.hpp"
#include "hta/ai/DynamicScene.hpp"
#include "hta/m3d/cmn/XmlFile.hpp"
#include "hta/m3d/cmn/XmlNode.hpp"
#include "ext/ai/Appendix.hpp"
#include "config.hpp"
#include "routines.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <Windows.h>

using namespace hta;

namespace hta::ai {
    struct DynamicScene;
    extern DynamicScene* gDynamicScene;

    // Ported from Meridian ai::CalcHitValue / ai::CalcSideCoeff (the originals at
    // 0x8C28F0 / 0x8C26B0 are Meridian-only and read Meridian config globals — those
    // addresses are DirectShow code in HTA, hence the previous crash). The two curves
    // are reproduced here with parameters read from kraken.ini [thorncollide].

    // Below a speed threshold there is no damage; above it, damage is linear in speed.
    static inline float CalcHitValue(float speed) {
        const kraken::Config& cfg = kraken::Config::Instance();
        if (speed < cfg.thorn_hit_speed_threshold.value) {
            return 0.0f;
        }
        return cfg.thorn_hit_damage_coeff.value * speed;
    }

    // Head-on hits (dot above threshold) use one coefficient, glancing hits another.
    static inline float CalcSideCoeff(float dot) {
        const kraken::Config& cfg = kraken::Config::Instance();
        return (dot > cfg.thorn_side_threshold.value)
            ? cfg.thorn_side_front_coeff.value
            : cfg.thorn_side_glancing_coeff.value;
    }
}

namespace {
    static auto dBodyGetPointVelPtr = reinterpret_cast<void(__fastcall*)(dxBody*, float, float, float, float*)>(0x007C49E0);

    // --- Per-vehicle ram strength (Meridian m_hitForce) -----------------------------
    // Meridian scaled ram damage by a per-vehicle hitForce read from the prototype
    // (ai::VehiclePrototypeInfo::m_hitForce, proto+0x4c, set by the XML "HitForce"
    // attribute; e.g. Scout=25, Tank=200). HTA's VehiclePrototypeInfo has no such field
    // (it rams by GetMass()), so we re-introduce the value: a side table keyed by the
    // (concrete) prototype object, populated from XML at load time via the LoadFromXML
    // trampoline below. The factory built-in carries Meridian's stock numbers so no
    // vehicles.xml edits are required; a modder may still override a vehicle with an
    // explicit HitForce="N" attribute, and ParentPrototype chains inherit the parent's
    // value (e.g. Scout01 -> Scout = 25).
    static const float kDefaultHitForce = 25.0f; // Meridian Scout baseline

    static const std::unordered_map<std::string, float>& MeridianHitForceTable() {
        static const std::unordered_map<std::string, float> table = {
            { "Scout",           25.0f  }, { "RoboScout",       10.0f  },
            { "Bug",             25.0f  }, { "Fighter",         50.0f  },
            { "Hunter",          75.0f  }, { "Cruiser",         150.0f },
            { "Dozer",           75.0f  }, { "Tank",            200.0f },
            { "Dot",             50.0f  }, { "Molokovoz",       50.0f  },
            { "Ural",            100.0f }, { "Belaz",           150.0f },
            { "Mirotvorec",      200.0f },
            { "Sml1",            10.0f  }, { "Sml2",            10.0f  },
            { "Sml3",            10.0f  }, { "Sml4",            10.0f  },
            { "Robot01",         100.0f }, { "Robot02",         150.0f },
            { "Robot03_Big",     150.0f }, { "Robot04_SMALL",   75.0f  },
            { "Robot_Player_01", 100.0f },
        };
        return table;
    }

    static std::unordered_map<const void*, float> g_hitForceByProto;
    static std::unordered_map<std::string, float> g_hitForceByName;

    static float GetPrototypeHitForce(const ai::VehiclePrototypeInfo* prototype) {
        if (!prototype) {
            return kDefaultHitForce;
        }
        auto it = g_hitForceByProto.find(prototype);
        return it != g_hitForceByProto.end() ? it->second : kDefaultHitForce;
    }

    // Resolve a prototype's hitForce while it is being loaded from XML: an explicit
    // HitForce attribute wins (modder override), else the built-in Meridian value for the
    // prototype's Name, else the value already resolved for its ParentPrototype (parents
    // load first), else the default.
    static float ResolveHitForce(const hta::m3d::cmn::XmlNode* node) {
        if (const char* s = node->GetAttribute("HitForce")) {
            return static_cast<float>(std::atof(s));
        }
        if (const char* name = node->GetAttribute("Name")) {
            const auto& table = MeridianHitForceTable();
            auto t = table.find(name);
            if (t != table.end()) {
                return t->second;
            }
        }
        if (const char* parent = node->GetAttribute("ParentPrototype")) {
            auto p = g_hitForceByName.find(parent);
            if (p != g_hitForceByName.end()) {
                return p->second;
            }
        }
        return kDefaultHitForce;
    }

    using VehicleLoadXmlFn = bool(__fastcall*)(hta::ai::VehiclePrototypeInfo*, void*,
                                               hta::m3d::cmn::XmlFile*, const hta::m3d::cmn::XmlNode*);
    static uint8_t s_vehicleLoadXmlTramp[16];

    bool __fastcall VehicleLoadFromXML_Hook(hta::ai::VehiclePrototypeInfo* self, void* edx,
                                            hta::m3d::cmn::XmlFile* xmlFile,
                                            const hta::m3d::cmn::XmlNode* xmlNode) {
        bool result = reinterpret_cast<VehicleLoadXmlFn>(static_cast<void*>(s_vehicleLoadXmlTramp))(
            self, edx, xmlFile, xmlNode);

        if (xmlNode) {
            const float hitForce = ResolveHitForce(xmlNode);
            if (const char* name = xmlNode->GetAttribute("Name")) {
                g_hitForceByName[name] = hitForce;
            }
            g_hitForceByProto[self] = hitForce;
        }
        return result;
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
    // Output channel for the "damage dealt to v2" value. Like Meridian, our ram-damage
    // routine REPLACES the engine's ai::CalcDamageToVehicles (0x0088F700) globally (see
    // Apply), so BOTH collision paths use it: vehicle-vs-vehicle (CollideVehiclePartAndVehiclePart)
    // and vehicle-vs-landscape (CollideVehicleAndLandscape). The engine ABI is
    // `void __fastcall(v1, v2, contacts, dSpeed, damageInfo, contactPos)` — 6 args,
    // ret 0x10 — and the native landscape caller relies on that exact signature (it
    // ignores the result), so we must keep it void/6-arg or the stack cleanup is off and
    // corrupts the caller. The v1 damage is written into damageInfo; the v2 damage is
    // published here for our own CollideVehiclePartAndVehiclePart to read.
    static float g_thornDamageToV2 = 0.0f;

    void __fastcall CalcDamageToVehicles(
        ai::Vehicle *v1,
        ai::Vehicle *v2,
        dContact *contacts,
        float *dSpeed,
        ai::DamageInfo *damageInfo,
        CVector * /*contactPos*/)
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

        g_thornDamageToV2 = static_cast<float>(damageToV2);
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

        CalcDamageToVehicles(
            (ai::Vehicle*)owner1,
            isVehicle2 ? (ai::Vehicle*)owner2 : nullptr,
            contacts,
            &deltaSpeed,
            &damageInfo,
            nullptr
        );
        float damageToVehicle2 = g_thornDamageToV2;

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
        // Replace the engine's ram-damage model with Meridian's globally. Meridian's
        // CalcDamageToVehicles (linear hit curve + side coefficient + thorn force,
        // asymmetric per-vehicle damage) is used by BOTH collision paths, so we must
        // override the engine function (0x0088F700) — not just our v-v handler — or
        // vehicle-vs-landscape would keep HTA's different (quadratic, symmetric, no
        // thorns) curve via CollideVehicleAndLandscape. The 6-arg void ABI is preserved
        // so that native caller's stack cleanup stays correct.
        kraken::routines::Redirect(0x383, (void*)0x0088F700, (void*)&CalcDamageToVehicles);
        kraken::routines::Redirect(0x34A, (void*)0x00890430, (void*)&CollideVehiclePartAndVehiclePart);

        // Capture each vehicle's Meridian hitForce as its prototype loads. Trampoline
        // ai::VehiclePrototypeInfo::LoadFromXML (RVA 0x1e9ce0); the prologue is
        // 83 EC 60 8B 44 24 64 (sub esp,0x60; mov eax,[esp+0x64]) — 7 position-independent
        // bytes, so we copy 7 (a 5-byte copy would split the mov).
        void* const origVehicleLoadXml = reinterpret_cast<void*>(0x005E9CE0);
        DWORD oldProt;
        VirtualProtect(s_vehicleLoadXmlTramp, sizeof(s_vehicleLoadXmlTramp), PAGE_EXECUTE_READWRITE, &oldProt);
        std::memcpy(s_vehicleLoadXmlTramp, origVehicleLoadXml, 7);
        s_vehicleLoadXmlTramp[7] = 0xE9;
        *reinterpret_cast<int32_t*>(s_vehicleLoadXmlTramp + 8) = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(origVehicleLoadXml) + 7
            - (reinterpret_cast<uintptr_t>(s_vehicleLoadXmlTramp) + 12));
        kraken::routines::Redirect(7, origVehicleLoadXml, reinterpret_cast<void*>(&VehicleLoadFromXML_Hook));
    }
}