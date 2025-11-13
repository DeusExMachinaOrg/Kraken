#define LOGGER "schwarz"

#include "configstructs.hpp"
#include "ext/logger.hpp"
#include "routines.hpp"

#include <unordered_map>
#include <unordered_set>

#include "hta/CStr.hpp"
#include "hta/ai/ComplexPhysicObj.hpp"
#include "hta/ai/CompoundGun.hpp"
#include "hta/ai/DynamicQuestPeace.hpp"
#include "hta/ai/DynamicScene.hpp"
#include "hta/ai/IPriceCoeffProvider.hpp"
#include "hta/ai/ObjContainer.hpp"
#include "hta/ai/Player.hpp"
#include "hta/ai/PrototypeManager.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/ai/VehiclePart.hpp"

#include "fix/complexschwarz.hpp"

enum GadgetId : __int32 // borrowed from CabinWnd::GadgetId
{
    GADGET_COMMON_MIN = 0x0,
    GADGET_COMMON_MAX = 0x4,
    GADGET_WEAPON_MIN = 0x5,
    GADGET_WEAPON_MAX = 0x9,
    GADGET_NUM_GADGETS = 0xA,
};

namespace {
    const std::unordered_set<std::string_view> significant_modifiers = {
        "Durability",
        "MaxDurability", // cargo and cabin
        "MaxTorque",
        "MaxSpeed",  // cabin
        "MaxHealth", // chassis
        "Damage",
        "FiringRate",
        "ReChargingTime",
        "Accuracy",
        "FiringRange",             // guns
        "GadgetAntiMissileRadius", // common
    };
}

namespace kraken::fix::complexschwarz {
    std::unordered_map<std::string, uint32_t, std::hash<std::string_view>, std::equal_to<>> schwarz_overrides;
    bool complex_schwarz{false};
    float gun_gadgets_max_schwarz_part{};
    float common_gadgets_max_schwarz_part{};
    float wares_max_schwarz_part{};

    bool peace_price_from_schwarz{false};
    bool no_money_in_player_schwarz{false};

    // TODO: add option to take into account how effective the gadget is for vehicle
    uint32_t GetCommonGadgetSchwarz(hta::ai::Gadget* gadget, const int gadget_id) {
        unsigned int gprice = gadget->GetPrice(0);
        const hta::ai::GadgetPrototypeInfo* gprotinfo = gadget->GetPrototypeInfo();
        const hta::CStr& modificator = gprotinfo->m_modifications[0].m_propertyName;

        LOG_INFO("--- Common Gadget #%d (1st mod: %s), RawPrice: %d ---", gadget->m_slotNum, modificator.m_charPtr, gprice);

        // TODO: allow to skip
        for (auto& modification : gprotinfo->m_modifications) {
            if (significant_modifiers.find(modification.m_propertyName) != significant_modifiers.end()) {
                return gprice;
            } else {
                continue;
            }
        }
        LOG_WARNING("Common Gadget ignored in schwarz, doesn't have significant modifiers");
        return 0;

        // | GADGETS AFFECTING BALANCE |
        // Vehicle (GADGET_COMMON)
        // GadgetAntiMissileRadius

        // Gun (GADGET_WEAPON)
        // Damage FiringRate ReChargingTime Accuracy FiringRange

        // VehicleParts (GADGET_COMMON)
        // MaxDurability Durability
        // [Chassis]
        // MaxFuel MaxHealth
        // [Cabin]
        // MaxSpeed MaxTorque [!FuelConsumption] [!Control]
    }

    // TODO: add option to take into account how effective the gadget is for guns config
    uint32_t GetWeaponGadgetSchwarz(hta::ai::Gadget* gadget, const int gadget_id) {
        uint32_t gprice = gadget->GetPrice(0);
        const hta::ai::GadgetPrototypeInfo* gprotinfo = gadget->GetPrototypeInfo();
        const hta::CStr& modificator = gprotinfo->m_modifications[0].m_propertyName;

        LOG_INFO("--- Weapon Gadget id #%d (1st mod: %s), RawPrice: %d ---", gadget->m_slotNum, modificator.m_charPtr, gprice);

        // TODO: allow to skip
        for (auto& modification : gprotinfo->m_modifications) {
            if (significant_modifiers.find(modification.m_propertyName) != significant_modifiers.end()) {
                return gprice;
            } else {
                continue;
            }
        }
        LOG_WARNING("Weapon Gadget ignored in schwarz, doesn't have significant modifiers");
        return 0;
    }

    uint32_t GetOverridenPrice(hta::ai::Obj* object) {
        const hta::CStr& prot_name = object->GetPrototypeInfo()->m_prototypeName;
        auto overriden_schwarz = schwarz_overrides.find(prot_name);
        if (overriden_schwarz != schwarz_overrides.end()) {
            LOG_DEBUG("Override price provided: %d", overriden_schwarz->second);
            return overriden_schwarz->second;
        }
        return 0;
    }

    uint32_t GetCompoundGunPartPrice(hta::ai::CompoundGun* gun) {
        float part_durability = gun->GetDurability();
        float part_max_durability = gun->GetMaxDurability();

        float condition = part_durability / part_max_durability;

        if (condition <= 0.01) {
            LOG_WARNING("Ignoring in Schwarz calculation, very low condition");
            return 0;
        }

        uint32_t base_price{};
        uint32_t overriden_base_schwarz = GetOverridenPrice(gun);
        if (overriden_base_schwarz != 0) {
            base_price = overriden_base_schwarz;
        } else {
            base_price = (uint32_t)gun->m_price.m_value;
        }

        uint32_t shells_price{};
        if (gun->IsWithCharging()) {
            if (gun->IsWithShellsPoolLimit()) {
                uint32_t shell_prot_id = gun->GetShellPrototypeId();
                if (shell_prot_id != -1) {
                    hta::ai::PrototypeInfo* shell_prototype;
                    if (!hta::ai::PrototypeManager::Instance->m_prototypes.empty() && shell_prot_id < hta::ai::PrototypeManager::Instance->m_prototypes.size())
                        shell_prototype = hta::ai::PrototypeManager::Instance->m_prototypes[shell_prot_id];
                    else
                        shell_prototype = 0;

                    if (shell_prototype) {
                        uint32_t base_shell_price = shell_prototype->m_price;
                        uint32_t shells_in_pool = gun->GetShellsInPool();
                        uint32_t shells_total = shells_in_pool + gun->GetShellsInCurrentCharge();
                        shells_price = base_shell_price * shells_total;
                    }
                }
            }
        }
        LOG_DEBUG("[Base: %d, Shells: %d] Dur: %.2f, MaxDur: %.2f, CompoundGun Condition: %.2f (condition ignored)", base_price, shells_price, part_durability, part_max_durability, condition);

        return base_price + shells_price;
    }

    uint32_t GetGunPartPrice(hta::ai::Gun* gun) {
        float part_durability = gun->m_durability.m_value.m_value;
        float part_max_durability = gun->m_durability.m_maxValue.m_value;

        float condition = part_durability / part_max_durability;

        if (condition <= 0.01) {
            LOG_WARNING("Ignoring in Schwarz calculation, very low condition");
            return 0;
        }

        uint32_t base_price{};
        uint32_t overriden_base_schwarz = GetOverridenPrice(gun);
        if (overriden_base_schwarz != 0) {
            base_price = overriden_base_schwarz;
        } else {
            base_price = (uint32_t)gun->m_price.m_value;
        }

        uint32_t shells_price{};
        if (gun->GetPrototypeInfo()->m_WithShellsPoolLimit) {
            uint32_t shell_prot_id = gun->m_shellPrototypeId;
            if (shell_prot_id != -1) {
                hta::ai::PrototypeInfo* shell_prototype;
                if (!hta::ai::PrototypeManager::Instance->m_prototypes.empty() && shell_prot_id < ai::PrototypeManager::Instance->m_prototypes.size())
                    shell_prototype = hta::ai::PrototypeManager::Instance->m_prototypes[shell_prot_id];
                else
                    shell_prototype = 0;

                if (shell_prototype) {
                    uint32_t base_shell_price = shell_prototype->m_price;
                    uint32_t shells_in_pool = gun->m_ShellsInPool;
                    uint32_t shells_total = shells_in_pool + gun->m_ShellsInCurrentCharge;
                    shells_price = base_shell_price * shells_total;
                }
            }
        }
        LOG_DEBUG("[Base: %d, Shells: %d] Dur: %.2f, MaxDur: %.2f, Gun Condition: %.2f (condition ignored)", base_price, shells_price, part_durability, part_max_durability, condition);

        return base_price + shells_price;
    }

    uint32_t GetPartPrice(hta::ai::VehiclePart* veh_part) {
        uint32_t overriden_base_schwarz = GetOverridenPrice(veh_part);
        if (overriden_base_schwarz != 0) {
            return overriden_base_schwarz;
        }
        return (uint32_t)veh_part->m_price.m_value;
    }

    uint32_t GetGadgetPrice(hta::ai::Gadget* gadget) {
        uint32_t overriden_base_schwarz = GetOverridenPrice(gadget);
        if (overriden_base_schwarz != 0) {
            return overriden_base_schwarz;
        }
        return gadget->GetPrototypeInfo()->m_price;
    }

    uint32_t GetDurablePartSchwarz(hta::ai::VehiclePart* veh_part) {
        float part_max_durability = veh_part->m_durability.m_maxValue.m_value;
        if (part_max_durability < 0.001) {
            LOG_WARNING("Part '%s' has ~0 max durability and is not CHASSIS, so will not be correctly processed", veh_part->m_partName.m_charPtr);

            return 1;
        }
        float part_durability = veh_part->m_durability.m_value.m_value;
        float condition_coeff = part_durability / part_max_durability;
        uint32_t base_price = GetPartPrice(veh_part);
        LOG_DEBUG("[Final: %.0f, Base: %d] Dur: %.2f, MaxDur: %.2f, PriceCoeff: %.2f", base_price * condition_coeff, base_price, part_durability, part_max_durability, condition_coeff);
        return (uint32_t)(base_price * condition_coeff);
    }

    uint32_t GetSchwarzOld(hta::ai::Vehicle* vehicle) {
        LOG_DEBUG("> GetSchwarz Original <");

        uint32_t vanilla_schwarz{};
        uint32_t vanilla_base_price = vehicle->GetPrice(0);

        float full_max_durability{};
        float full_cur_durability{};

        full_max_durability = vehicle->GetMaxFullDurability();
        full_cur_durability = vehicle->GetFullDurability();

        float full_dur_price_coeff{};
        if (full_max_durability >= 0.001)
            full_dur_price_coeff = full_cur_durability / full_max_durability;

        vanilla_schwarz = (uint32_t)(vanilla_base_price * full_dur_price_coeff);

        LOG_WARNING("----- Vanilla Schwarz: %d, Base Price: %d, dur_coeff: %.2f (~%.2f/%.2f) -----", vanilla_schwarz, vanilla_base_price, full_dur_price_coeff, full_cur_durability, full_max_durability);
        return vanilla_schwarz;
    }

    uint32_t __fastcall GetComplexSchwarz(hta::ai::Vehicle* vehicle, void* _) {
        if (!complex_schwarz) {
            return GetSchwarzOld(vehicle);
        }

        LOG_DEBUG("> GetComplexSchwarz <");
        LOG_WARNING("----- %s -----", vehicle->m_name.m_charPtr);

        uint32_t intermediate_schwarz{};

        uint32_t cab_price{};
        uint32_t basket_price{};
        uint32_t chassis_price{};
        uint32_t guns_schwarz{};

        for (const auto& [part_name, veh_part] : vehicle->m_vehicleParts) {
            LOG_INFO("--- [%s] %s ---", part_name.m_charPtr, veh_part->GetPrototypeInfo()->m_prototypeName.m_charPtr);

            if (part_name.Equal("CHASSIS")) {
                float condition_coeff = vehicle->GetHealth() / vehicle->GetMaxHealth();
                uint32_t base_price = GetPartPrice(veh_part); // TODO: remove additional vars with debug logs
                chassis_price += (uint32_t)(base_price * condition_coeff);
                LOG_DEBUG("[Final: %.0f, Base: %d] HP: %.2f, MaxHP: %.2f, PriceCoeff: %.2f", base_price * condition_coeff, base_price, vehicle->GetHealth(), vehicle->GetMaxHealth(), condition_coeff);
            } else if (part_name.Equal("CABIN")) {
                cab_price += GetDurablePartSchwarz(veh_part);
            } else if (part_name.Equal("BASKET")) {
                basket_price += GetDurablePartSchwarz(veh_part);
            } else if (veh_part->IsKindOf((hta::m3d::Class*)0x00A024D0)) // ai::CompoundGun*
            {
                guns_schwarz += GetCompoundGunPartPrice(reinterpret_cast<hta::ai::CompoundGun*>(veh_part));
            } else // a normal gun
            {
                // m3d::Class* pclass = veh_part->GetClass();
                guns_schwarz += GetGunPartPrice(reinterpret_cast<hta::ai::Gun*>(veh_part));
            }
        }

        uint32_t gun_gadgets_price{};
        uint32_t common_gadgets_price{};

        for (const auto& [gadget_id, gadget] : vehicle->m_gadgets) {
            // gadget_id/slot 0 - 4 (COMMON), slot 5 - 9 (WEAPON)
            if (gadget_id <= GadgetId::GADGET_COMMON_MAX) {
                common_gadgets_price += GetCommonGadgetSchwarz(gadget, gadget_id);
            } else {
                gun_gadgets_price += GetWeaponGadgetSchwarz(gadget, gadget_id);
            }
        }

        uint32_t wares_price{};

        if (vehicle->m_repository) {
            uint32_t num_items = vehicle->m_repository->m_slots.size();

            if (num_items) {
                for (auto& item : vehicle->m_repository->m_slots) {
                    hta::ai::Obj* obj = item.GetObj();
                    if (obj) {
                        wares_price += obj->GetPrice(0);
                    }
                }
            }
        }

        intermediate_schwarz = cab_price + basket_price + chassis_price;
        LOG_DEBUG("Only cab+cargo+chassis: %d", intermediate_schwarz);

        uint32_t total_gadgets_schwarz{};
        uint32_t total_wares_schwarz{};
        if (gun_gadgets_price) {
            uint32_t max_gun_gadget_schwarz = (uint32_t)(guns_schwarz * gun_gadgets_max_schwarz_part);
            total_gadgets_schwarz += min(gun_gadgets_price, max_gun_gadget_schwarz);
            LOG_DEBUG("[WeaponGadgets final: %d, Base: %d] WeaponGadgetMaxSchwarzPart (from base vehicle): %.2f", min(gun_gadgets_price, max_gun_gadget_schwarz), gun_gadgets_price, gun_gadgets_max_schwarz_part);
        }
        if (common_gadgets_price) {
            uint32_t max_common_gadget_schwarz = (uint32_t)(intermediate_schwarz * common_gadgets_max_schwarz_part);
            total_gadgets_schwarz += min(common_gadgets_price, max_common_gadget_schwarz);
            LOG_DEBUG("[CommonGadgets final: %d, Base: %d] CommonGadgetMaxSchwarzPart (from base vehicle): %.2f", min(common_gadgets_price, max_common_gadget_schwarz), // remove all debug duplication of calculations
                      common_gadgets_price, max_common_gadget_schwarz);
        }

        if (wares_price) {
            uint32_t maximum_wares_part_of_schwarz = (uint32_t)(intermediate_schwarz * wares_max_schwarz_part);
            total_wares_schwarz = min(wares_price, maximum_wares_part_of_schwarz);
            LOG_DEBUG("[Wares final: %d, Base: %d] WaresMaxSchwarzPart: %.2f", total_wares_schwarz, wares_price, wares_max_schwarz_part);
            intermediate_schwarz += total_wares_schwarz;
        }

        intermediate_schwarz += guns_schwarz;

        GetSchwarzOld(vehicle);
        LOG_WARNING("----- New Schwarz: %d (Schwarz Parts: CAB %d + BASKET %d + CHASSIS %d + GUNS %d + GADGETS %d + WARES %d) -----", intermediate_schwarz, cab_price, basket_price, chassis_price, guns_schwarz, total_gadgets_schwarz,
                    total_wares_schwarz);

        return intermediate_schwarz;
    }

    int32_t __fastcall _CalcRewardForPeace(hta::ai::DynamicQuestPeace* quest, void* _) // ai::DynamicQuestPeace
    {
        int32_t calculated_from_player{};

        const hta::ai::DynamicQuestPeacePrototypeInfo* protInfo = quest->GetPrototypeInfo();
        if (!hta::ai::DynamicScene::Instance()->GetVehicleControlledByPlayer())
            return protInfo->m_minReward;
        if (!hta::ai::Player::Instance)
            return 0;

        if (peace_price_from_schwarz)
            calculated_from_player = (int32_t)(hta::ai::Player::Instance->GetSchwarz() * protInfo->m_playerMoneyPart);
        else
            calculated_from_player = (int32_t)(hta::ai::Player::Instance->GetMoney() * protInfo->m_playerMoneyPart);

        return -min(calculated_from_player, protInfo->m_minReward);
    }

    uint32_t __fastcall GetPlayerSchwarz(hta::ai::Player* player, void* _) {
        uint32_t schwarz{};
        hta::ai::Vehicle* vehicle;

        schwarz = no_money_in_player_schwarz ? 0 : player->m_money.m_value.m_value;
        vehicle = player->GetVehicle();
        if (vehicle)
            schwarz += vehicle->GetSchwarz();
        return schwarz;
    }

    uint32_t __fastcall GetVehiclePartPrice(hta::ai::VehiclePart* veh_part, void* _, const hta::ai::IPriceCoeffProvider* provider) {
        float price;
        float condition_coeff{};
        float raw_price;

        raw_price = veh_part->m_price.m_value;
        if (veh_part->m_durability.m_maxValue.m_value >= 0.001f) // use durability by default
            condition_coeff = veh_part->m_durability.m_value.m_value / veh_part->m_durability.m_maxValue.m_value;
        else if (complex_schwarz) // when complex schwarz is disabled, we want to keep vanilla logic
        {
            if (veh_part->m_parentId != -1) // for hp condition checks we need parent vehicle
            {
                hta::ai::Obj* parent_obj = (hta::ai::Obj*)hta::ai::ObjContainer::theObjects->GetEntityByObjId(veh_part->m_parentId);
                if (parent_obj->IsKindOf((hta::m3d::Class*)0x00A00914)) // ai::Vehicle*
                {
                    float health = reinterpret_cast<hta::ai::Vehicle*>(parent_obj)->GetHealth();
                    float max_health = reinterpret_cast<hta::ai::Vehicle*>(parent_obj)->GetMaxHealth();
                    if (max_health >= 0.001f) // probably only true for chassis?
                    {
                        condition_coeff = health / max_health;
                    }
                }
            }
        }

        price = veh_part->GetPriceCoeff(provider) * condition_coeff * raw_price;
        return price >= 1.0 ? (uint32_t)price : 1;
    }

    void Apply() {
        const kraken::Config& config = kraken::Config::Get();

        LOG_INFO("Feature enabled");

        schwarz_overrides = config.schwarz_overrides.value;
        complex_schwarz = config.complex_schwarz.value;
        gun_gadgets_max_schwarz_part = config.gun_gadgets_max_schwarz_part.value;
        common_gadgets_max_schwarz_part = config.common_gadgets_max_schwarz_part.value;
        wares_max_schwarz_part = config.wares_max_schwarz_part.value;

        peace_price_from_schwarz = config.peace_price_from_schwarz.value;
        no_money_in_player_schwarz = config.no_money_in_player_schwarz.value;

        kraken::routines::Redirect(0x0088, (void*)0x005E0DB0, (void*)&GetComplexSchwarz);
        kraken::routines::Redirect(0x0088, (void*)0x00746D50, (void*)&_CalcRewardForPeace);
        kraken::routines::Redirect(0x0088, (void*)0x00651380, (void*)&GetPlayerSchwarz);
        kraken::routines::Redirect(0x0020, (void*)0x006CF0C0, (void*)&GetVehiclePartPrice);
    }
}