#define LOGGER "SCHWARZ"

#include <unordered_map>
#include <unordered_set>
#include "routines.hpp"
#include "configstructs.hpp"
#include "ext/logger.hpp"
#include "CStr.h"
#include "hta/ai/PrototypeManager.h"
#include "hta/ai/DynamicScene.hpp"
#include "hta/ai/Vehicle.h"
#include "hta/ai/VehiclePart.h"
#include "hta/ai/ComplexPhysicObj.h"
#include "fix/schwarzfix.hpp"
#include "hta/ai/DynamicQuestPeace.hpp"
#include "hta/ai/Player.hpp"

enum GadgetId : __int32 // borrowed from CabinWnd::GadgetId
{
  GADGET_COMMON_MIN = 0x0,
  GADGET_COMMON_MAX = 0x4,
  GADGET_WEAPON_MIN = 0x5,
  GADGET_WEAPON_MAX = 0x9,
  GADGET_NUM_GADGETS = 0xA,
};

std::unordered_set<std::string> significant_modifiers = {
    "Durability", "MaxDurability", // cargo and cabin
    "MaxTorque", "MaxSpeed", // cabin
    "MaxHealth", // chassis
    "Damage", "FiringRate", "ReChargingTime", "Accuracy", "FiringRange", // guns
    "GadgetAntiMissileRadius", // common
};

namespace kraken::fix::schwarzfix {
    static inline ai::PrototypeManager*& thePrototypeManager = *(ai::PrototypeManager**)0x00A18AA8;
    static inline ai::DynamicScene*& gDynamicScene = *(ai::DynamicScene**)0x00A12958;
    static inline ai::Player*& thePlayer = *(ai::Player**)0x00A135E4;


    std::unordered_map<std::string, uint32_t> schwarz_overrides;
    bool complex_schwarz{false};
    float gun_gadgets_max_schwarz_part{};
    float common_gadgets_max_schwarz_part{};
    float wares_max_schwarz_part{};

    bool peace_price_from_schwarz{false};
    bool no_money_in_player_schwarz{false};

    // TODO: add option to take into account how effective the gadget is for vehicle
    uint32_t GetCommonGadgetSchwarz(ai::Gadget* gadget, const int gadget_id)
    {
        unsigned int gprice = gadget->GetPrice(0);
        const ai::GadgetPrototypeInfo* gprotinfo = gadget->GetPrototypeInfo();
        CStr modificator = gprotinfo->m_modifications[0].m_propertyName;

        LOG_INFO("--- Common Gadget #%d (1st mod: %s), RawPrice: %d ---",
            gadget->m_slotNum,
            modificator.charPtr,
            gprice);

        // TODO: allow to skip
        for (auto& modification : gprotinfo->m_modifications)
        {
            if (significant_modifiers.find((std::string)modification.m_propertyName) != significant_modifiers.end())
            {
                return gprice;
            }
            else
            {
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
    uint32_t GetWeaponGadgetSchwarz(ai::Gadget* gadget, const int gadget_id)
    {
        uint32_t gprice = gadget->GetPrice(0);
        const ai::GadgetPrototypeInfo* gprotinfo = gadget->GetPrototypeInfo();
        CStr modificator = gprotinfo->m_modifications[0].m_propertyName;

        LOG_INFO("--- Weapon Gadget id #%d (1st mod: %s), RawPrice: %d ---",
            gadget->m_slotNum,
            modificator.charPtr,
            gprice);


        // TODO: allow to skip
        for (auto& modification : gprotinfo->m_modifications)
        {
            if (significant_modifiers.find((std::string)modification.m_propertyName) != significant_modifiers.end())
            {
                return gprice;
            }
            else
            {
                continue;
            }
        }
        LOG_WARNING("Weapon Gadget ignored in schwarz, doesn't have significant modifiers");
        return 0;

    }

    uint32_t GetOverridenPrice(ai::Obj* object)
    {
        CStr prot_name = object->GetPrototypeInfo()->m_prototypeName;
        auto overriden_schwarz = schwarz_overrides.find((std::string)prot_name);
        if (overriden_schwarz != schwarz_overrides.end())
        {
            LOG_DEBUG("Override price provided: %d", overriden_schwarz->second);
            return overriden_schwarz->second;
        }
        return 0;
    }

    uint32_t GetGunPartPrice(ai::Gun* gun)
    {
        float part_durability = gun->m_durability.m_value.m_value;
        float part_max_durability = gun->m_durability.m_maxValue.m_value;

        float condition = part_durability / part_max_durability;

        if (condition <= 0.01)
        {
            LOG_WARNING("Ignoring in Schwarz calculation, very low condition");
            return 0;
        }

        uint32_t base_price{};
        uint32_t overriden_base_schwarz = GetOverridenPrice(gun);
        if (overriden_base_schwarz != 0)
        {
            base_price = overriden_base_schwarz;
        }
        else
        {
            base_price = gun->GetPrice(0); // TODO: very much possible that this doesn't work correctly for Compound Guns
        }

        uint32_t shells_price{};
        if (gun->GetPrototypeInfo()->m_WithShellsPoolLimit)
        {
            uint32_t shell_prot_id = gun->m_shellPrototypeId;
            if (shell_prot_id != -1)
            {
                ai::PrototypeInfo* shell_prototype;
                if ( !thePrototypeManager->m_prototypes.empty() && shell_prot_id < thePrototypeManager->m_prototypes.size() )
                    shell_prototype = thePrototypeManager->m_prototypes[shell_prot_id];
                else
                    shell_prototype = 0;

                if (shell_prototype)
                {
                    uint32_t base_shell_price = shell_prototype->m_price;
                    uint32_t shells_in_pool = gun->m_ShellsInPool;
                    uint32_t shells_total = shells_in_pool + gun->m_ShellsInCurrentCharge;
                    shells_price = base_shell_price * shells_total;
                }
            }
        }
        LOG_DEBUG("[Base: %d, Shells: %d] Dur: %.2f, MaxDur: %.2f, Gun Condition: %.2f (condition ignored)",
            base_price, shells_price,
            part_durability,
            part_max_durability,
            condition);

        return base_price + shells_price;


    }

    uint32_t GetPartPrice(ai::VehiclePart* veh_part)
    {
        uint32_t overriden_base_schwarz = GetOverridenPrice(veh_part);
        if (overriden_base_schwarz != 0)
        {
            return overriden_base_schwarz;
        }
        return veh_part->m_price.m_value;
    }

    uint32_t GetGadgetPrice(ai::Gadget* gadget)
    {
        uint32_t overriden_base_schwarz = GetOverridenPrice(gadget);
        if (overriden_base_schwarz != 0)
        {
            return overriden_base_schwarz;
        }
    }

    uint32_t GetDurablePartSchwarz(ai::VehiclePart* veh_part)
    {
        float part_max_durability = veh_part->m_durability.m_maxValue.m_value;
        if (part_max_durability < 0.001)
        {
            LOG_WARNING(
                "Part '%s' has ~0 max durability and is not CHASSIS, so will not be correctly processed",
                veh_part->m_partName.charPtr);

            return 1;
        }
        float part_durability = veh_part->m_durability.m_value.m_value;
        float condition_coeff = part_durability / part_max_durability;
        uint32_t base_price = GetPartPrice(veh_part);
        LOG_DEBUG("[Final: %.0f, Base: %d] Dur: %.2f, MaxDur: %.2f, PriceCoeff: %.2f",
            base_price * condition_coeff,
            base_price,
            part_durability,
            part_max_durability,
            condition_coeff);
        return base_price * condition_coeff;
    }

    uint32_t GetSchwarzOld(ai::Vehicle* vehicle)
    {
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

        LOG_WARNING("----- Vanilla Schwarz: %d, Base Price: %d, dur_coeff: %.2f (~%.2f/%.2f) -----",
            vanilla_schwarz, vanilla_base_price, full_dur_price_coeff, full_cur_durability, full_max_durability);
        return vanilla_schwarz;
    }

    uint32_t __fastcall GetComplexSchwarz(ai::Vehicle* vehicle, void* _)
    {
        LOG_DEBUG("> GetComplexSchwarz <");
        LOG_WARNING("----- %s -----", vehicle->name);

        float intermediate_schwarz{};

        uint32_t cab_price{};
        uint32_t basket_price{};
        uint32_t chassis_price{};
        uint32_t guns_schwarz{};

        for (const auto& [part_name, veh_part] : vehicle->m_vehicleParts) {
            LOG_INFO("--- [%s] %s ---", part_name.charPtr, veh_part->GetPrototypeInfo()->m_prototypeName);

            if (part_name.Equal("CHASSIS"))
            {
                float condition_coeff = vehicle->GetHealth() / vehicle->GetMaxHealth();
                uint32_t base_price = GetPartPrice(veh_part); // TODO: remove additional vars with debug logs
                chassis_price += base_price * condition_coeff;
                LOG_DEBUG("[Final: %.0f, Base: %d] HP: %.2f, MaxHP: %.2f, PriceCoeff: %.2f",
                    base_price * condition_coeff,
                    base_price,
                    vehicle->GetHealth(),
                    vehicle->GetMaxHealth(),
                    condition_coeff);
            }
            else if (part_name.Equal("CABIN"))
            {
                cab_price += GetDurablePartSchwarz(veh_part);
            }
            else if (part_name.Equal("BASKET"))
            {
                basket_price += GetDurablePartSchwarz(veh_part);
            }
            else // a gun
            {
                guns_schwarz += GetGunPartPrice(reinterpret_cast<ai::Gun*>(veh_part));
            }
        }

        uint32_t gun_gadgets_price{};
        uint32_t common_gadgets_price{};

        for (const auto& [gadget_id, gadget] : vehicle->m_gadgets){
            // gadget_id/slot 0 - 4 (COMMON), slot 5 - 9 (WEAPON)
            if (gadget_id <= GadgetId::GADGET_COMMON_MAX)
            {
                common_gadgets_price += GetCommonGadgetSchwarz(gadget, gadget_id);
            }
            else
            {
                gun_gadgets_price += GetWeaponGadgetSchwarz(gadget, gadget_id);
            }
        }

        uint32_t wares_price{};

        if (vehicle->m_repository)
        {
            uint32_t num_items = vehicle->m_repository->m_slots.size();

            if (num_items)
            {
                for (auto& item : vehicle->m_repository->m_slots)
                {
                    ai::Obj* obj = item.GetObj();
                    if (obj)
                    {
                        wares_price += obj->GetPrice(0);
                    }
                }
            }

        }

        intermediate_schwarz = cab_price + basket_price + chassis_price;
        LOG_DEBUG("Only cab+cargo+chassis: %d", intermediate_schwarz);

        uint32_t total_gadgets_schwarz{};
        uint32_t total_wares_schwarz{};
        if (gun_gadgets_price)
        {
            uint32_t max_gun_gadget_schwarz = guns_schwarz * gun_gadgets_max_schwarz_part;
            total_gadgets_schwarz += min(gun_gadgets_price, max_gun_gadget_schwarz);
            LOG_DEBUG("[WeaponGadgets final: %d, Base: %d] WeaponGadgetMaxSchwarzPart (from base vehicle): %.2f",
                min(gun_gadgets_price, max_gun_gadget_schwarz),
                gun_gadgets_price,
                gun_gadgets_max_schwarz_part);
        }
        if (common_gadgets_price)
        {
            uint32_t max_common_gadget_schwarz = intermediate_schwarz * common_gadgets_max_schwarz_part;
            total_gadgets_schwarz += min(common_gadgets_price, max_common_gadget_schwarz);
            LOG_DEBUG("[CommonGadgets final: %d, Base: %d] CommonGadgetMaxSchwarzPart (from base vehicle): %.2f",
                min(common_gadgets_price, max_common_gadget_schwarz), // remove all debug duplication of calculations
                common_gadgets_price,
                max_common_gadget_schwarz);
        }

        if (wares_price)
        {
            uint32_t maximum_wares_part_of_schwarz = intermediate_schwarz * wares_max_schwarz_part;
            total_wares_schwarz = min(wares_price, maximum_wares_part_of_schwarz);
            LOG_DEBUG("[Wares final: %d, Base: %d] WaresMaxSchwarzPart: %.2f",
                total_wares_schwarz,
                wares_price,
                wares_max_schwarz_part);
            intermediate_schwarz += total_wares_schwarz;
        }

        intermediate_schwarz += guns_schwarz;

        GetSchwarzOld(vehicle);
        LOG_WARNING("----- New Schwarz: %.1f (Schwarz Parts: CAB %d + BASKET %d + CHASSIS %d + GUNS %d + GADGETS %d + WARES %d) -----",
            intermediate_schwarz, cab_price, basket_price, chassis_price, guns_schwarz, total_gadgets_schwarz, total_wares_schwarz);

        return (uint32_t)intermediate_schwarz;
    }

    int32_t __fastcall _CalcReward(ai::DynamicQuestPeace* quest, void* _) // ai::DynamicQuestPeace
    {
        int32_t calculated_from_player{};
      
        const ai::DynamicQuestPeacePrototypeInfo* protInfo = quest->GetPrototypeInfo();
        if ( !gDynamicScene->GetVehicleControlledByPlayer() )
            return protInfo->m_minReward;
        if ( !thePlayer )
            return 0;
        
        if (peace_price_from_schwarz)
            calculated_from_player = thePlayer->GetSchwarz() * protInfo->m_playerMoneyPart;
        else
            calculated_from_player = thePlayer->GetMoney() * protInfo->m_playerMoneyPart;
            

        return -min(calculated_from_player, protInfo->m_minReward);
    }

    uint32_t GetSchwarz(ai::Player* player) 
    {
        uint32_t schwarz{};
        ai::Vehicle *vehicle;

        schwarz = no_money_in_player_schwarz ? 0 : player->m_money.m_value.m_value;
        vehicle = player->GetVehicle();
        if ( vehicle )
            schwarz += vehicle->GetSchwarz();
        return schwarz;
    }

    void Apply() {
        const kraken::Config& config = kraken::Config::Get();
        schwarz_overrides = config.schwarz_overrides.value;
        complex_schwarz = config.complex_schwarz.value;
        gun_gadgets_max_schwarz_part = config.gun_gadgets_max_schwarz_part.value;
        common_gadgets_max_schwarz_part = config.common_gadgets_max_schwarz_part.value;
        wares_max_schwarz_part = config.wares_max_schwarz_part.value;

        peace_price_from_schwarz = config.peace_price_from_schwarz.value;
        no_money_in_player_schwarz = config.no_money_in_player_schwarz.value;

        kraken::routines::Redirect(0x0088, (void*) 0x005E0DB0, (void*) &GetComplexSchwarz);
    }
}
