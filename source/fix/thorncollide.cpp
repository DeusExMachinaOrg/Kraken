#define LOGGER "RAMCOLLIDE"

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
#include "ext/logger.hpp"
#include "config.hpp"
#include "routines.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <Windows.h>

using namespace hta;

namespace hta::ai {
    struct DynamicScene;
    extern DynamicScene* gDynamicScene;
}

namespace {
    static auto dBodyGetPointVelPtr = reinterpret_cast<void(__fastcall*)(dxBody*, float, float, float, float*)>(0x007C49E0);

    // --- Per-vehicle ram offense ----------------------------------------------------
    // In the ram-damage model mass carries the size/weight dynamic, so a vehicle's
    // "offense" defaults to 1.0 (neutral). A modder may make a specific vehicle a harder
    // rammer (reinforced bull-bar) with an XML attribute RamOffense="N"; ParentPrototype
    // chains inherit it (parents load first). Captured per prototype via the LoadFromXML
    // trampoline below. (Thorn melee weapons are a separate multiplier, see ThornMultiplier.)
    static const float kDefaultRamOffense = 1.0f;

    static std::unordered_map<const void*, float> g_ramOffenseByProto;
    static std::unordered_map<std::string, float> g_ramOffenseByName;

    static float GetPrototypeRamOffense(const ai::VehiclePrototypeInfo* prototype) {
        if (!prototype) {
            return kDefaultRamOffense;
        }
        auto it = g_ramOffenseByProto.find(prototype);
        return it != g_ramOffenseByProto.end() ? it->second : kDefaultRamOffense;
    }

    static float ResolveRamOffense(const hta::m3d::cmn::XmlNode* node) {
        const char* name = node->GetAttribute("Name");
        const char* protoName = name ? name : "<noname>";

        if (const char* s = node->GetAttribute("RamOffense")) {
            const float value = static_cast<float>(std::atof(s));
            LOG_DEBUG("vehicle '%s': RamOffense = %.3f (from XML)", protoName, value);
            return value;
        }
        if (const char* parent = node->GetAttribute("ParentPrototype")) {
            auto p = g_ramOffenseByName.find(parent);
            if (p != g_ramOffenseByName.end()) {
                LOG_DEBUG("vehicle '%s': RamOffense = %.3f (inherited from ParentPrototype '%s', no own attr)",
                          protoName, p->second, parent);
                return p->second;
            }
        }
        LOG_DEBUG("vehicle '%s': RamOffense = %.3f (DEFAULT, no XML attr)", protoName, kDefaultRamOffense);
        return kDefaultRamOffense;
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
            const float offense = ResolveRamOffense(xmlNode);
            if (const char* name = xmlNode->GetAttribute("Name")) {
                g_ramOffenseByName[name] = offense;
            }
            g_ramOffenseByProto[self] = offense;
        }
        return result;
    }

    static inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

    // Directional armor: how square the hit lands on a vehicle's own orientation.
    // cosForward = dot(forward, dirFromCenterToContact): +1 front, 0 side, -1 rear.
    static float DirectionalArmor(float cosForward) {
        const kraken::Config& cfg = kraken::Config::Instance();
        if (cosForward >= 0.0f) {
            return Lerp(cfg.ram_armor_side.value, cfg.ram_armor_front.value, cosForward);
        }
        return Lerp(cfg.ram_armor_side.value, cfg.ram_armor_rear.value, -cosForward);
    }

    // GetVehicleThornForce returns 1.0 for a vehicle without a thorn, else the equipped
    // appendix's ThornForce (25/50/75). Map that to a modest melee multiplier.
    static float ThornMultiplier(float thornForce) {
        if (thornForce <= 1.0f) {
            return 1.0f;
        }
        return 1.0f + thornForce * kraken::Config::Instance().ram_thorn_scale.value;
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

    // ===========================================================================
    //  RAM DAMAGE — final formula (replaces HTA's stock ai::CalcDamageToVehicles)
    // ---------------------------------------------------------------------------
    //  For a contact between vehicles V1 and V2 (V2 absent => static landscape):
    //
    //    v       = |dot(contactNormal, pointVel1 - pointVel2)|   // normal approach speed
    //    vEff    = max(0, v - speed_threshold)
    //    term    = damage_coeff * vEff ^ speed_exponent          // p=1 impulse, 2 energy
    //
    //    m_i     = Vi.GetMass()
    //    share1  = m2 / (m1 + m2)      share2 = m1 / (m1 + m2)    // landscape: 1 and 0
    //
    //    offense_i = RamOffense(Vi)  (default 1.0; static => landscape_offense)
    //    thorn_i   = 1 + ThornForce(Vi) * thorn_scale            // 1.0 if no thorn
    //    armor_i   = directional armor at the hit point, lerp over
    //                cos(forward_i, dirToContact_i): front / side / rear
    //
    //    damageToV1 = clamp( term * share1 * offense2 * thorn2 * armor1, 0, max_damage )
    //    damageToV2 = clamp( term * share2 * offense1 * thorn1 * armor2, 0, max_damage )
    //
    //  Defense is NOT in this formula: the engine's Vehicle::InflictDamage scales the
    //  result by the target's blast resistance (damage type = DAMAGE_BLAST).
    //  All coefficients live in kraken.ini [thorncollide]; see kraken::Config (ram_*).
    // ===========================================================================
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

        // 5. Тяжесть удара: нормальная скорость сближения за вычетом порога, возведённая
        //    в степень p (1 = импульс, 2 = энергия), умноженная на общий коэффициент.
        *dSpeed = fabs(CVector::Dot(contactNormal, deltaVel));
        const kraken::Config& cfg = kraken::Config::Instance();
        const float vEff = fmaxf(0.0f, *dSpeed - cfg.ram_speed_threshold.value);
        const float speedTerm = (vEff > 0.0f)
            ? cfg.ram_damage_coeff.value * powf(vEff, cfg.ram_speed_exponent.value)
            : 0.0f;

        // 6. Массовые доли (приведённая масса): более лёгкое тело поглощает большую долю.
        //    Против статики (нет v2) вся доля у v1, препятствие не получает ничего.
        const float m1 = v1->GetMass();
        float share1 = 1.0f;
        float share2 = 0.0f;
        if (isVehicle2 && v2) {
            const float m2 = v2->GetMass();
            const float total = m1 + m2;
            if (total > 1e-3f) {
                share1 = m2 / total;
                share2 = m1 / total;
            } else {
                share1 = share2 = 0.5f;
            }
        }

        // 7. Направленная броня каждой машины в точке удара (лоб крепче, борт/корма слабее).
        CVector v1Dir = v1->GetDirection();
        const float armor1 = DirectionalArmor(CVector::Dot(v1Dir, dirToContact1));
        float armor2 = 1.0f;
        if (isVehicle2 && v2) {
            CVector pos2 = v2->GetPosition();
            CVector dirToContact2 = NormalizeSafe(contactPos - pos2);
            CVector v2Dir = v2->GetDirection();
            armor2 = DirectionalArmor(CVector::Dot(v2Dir, dirToContact2));
        }

        // 8. Агрессия тарана (дефолт 1.0; у статики — константа из конфига) + множитель шипов.
        const float offense1 = isVehicle1 ? GetPrototypeRamOffense(v1->GetPrototypeInfo()) : cfg.ram_landscape_offense.value;
        const float offense2 = isVehicle2 ? GetPrototypeRamOffense(v2->GetPrototypeInfo()) : cfg.ram_landscape_offense.value;
        const float thorn1 = isVehicle1 ? ThornMultiplier(kraken::ext::ai::GetVehicleThornForce(v1)) : 1.0f;
        const float thorn2 = isVehicle2 ? ThornMultiplier(kraken::ext::ai::GetVehicleThornForce(v2)) : 1.0f;

        // 9. Урон каждой машине = тяжесть * своя массовая доля * (offense*thorn атакующего)
        //    * своя направленная броня, с ограничением. Сопротивление цели применит позже
        //    InflictDamage (тип урона BLAST).
        float damageToV1 = speedTerm * share1 * offense2 * thorn2 * armor1;
        float damageToV2 = speedTerm * share2 * offense1 * thorn1 * armor2;
        damageToV1 = fminf(fmaxf(damageToV1, 0.0f), cfg.ram_max_damage.value);
        damageToV2 = fminf(fmaxf(damageToV2, 0.0f), cfg.ram_max_damage.value);

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