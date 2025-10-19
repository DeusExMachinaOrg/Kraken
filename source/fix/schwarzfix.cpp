#define LOGGER "SCHWARZ"

#include "hta/ai/Vehicle.h"
#include "hta/ai/VehiclePart.h"
#include "hta/ai/ComplexPhysicObj.h"
#include "fix/schwarzfix.hpp"
#include "routines.hpp"
#include "ext/logger.hpp"
#include <unordered_map>
#include <unordered_set>
#include "CStr.h"

enum GadgetId : __int32 // borrowed from CabinWnd::GadgetId
{
  GADGET_COMMON_MIN = 0x0,
  GADGET_COMMON_MAX = 0x4,
  GADGET_WEAPON_MIN = 0x5,
  GADGET_WEAPON_MAX = 0x9,
  GADGET_NUM_GADGETS = 0xA,
};

std::unordered_set<CStr> significant_modifiers = {
    "Durability", "MaxDurability", // cargo and cabin
    "MaxTorque", "MaxSpeed", // cabin
    "MaxHealth", // chassis
    "Damage", "FiringRate", "ReChargingTime", "Accuracy", "FiringRange", // guns
};

namespace kraken::fix::schwarzfix {

    uint32_t GetCommonGadgetSchwarz(ai::Gadget* gadget, const int gadget_id, stable_size_map<CStr, ai::VehiclePart*> vehicle_parts)
    {
        unsigned int gprice = gadget->GetPrice(0);
        const ai::GadgetPrototypeInfo* gprotinfo = gadget->GetPrototypeInfo();
        CStr modificator = gprotinfo->m_modifications[0].m_propertyName;

        LOG_INFO("--- Common Gadget id%d #%d (1st mod: %s), Price: %d ---",
            gadget_id,
            gadget->m_slotNum,
            modificator.charPtr,
            gprice);

        for (auto modification : gprotinfo->m_modifications)
        {
            if (significant_modifiers.find(modification.m_propertyName.charPtr) != significant_modifiers.end())
            {
                return gadget->GetPrice(0);
            }
            else
            {
                continue;
            }
        }
        return 0;

        for (const auto[part_name, veh_part] : vehicle_parts)
        {

        }

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

    uint32_t GetWeaponGadgetSchwarz(ai::Gadget* gadget, const int gadget_id, std::unordered_map<CStr, ai::VehiclePart*> weapons)
    {
        uint32_t gprice = gadget->GetPrice(0);
        const ai::GadgetPrototypeInfo* gprotinfo = gadget->GetPrototypeInfo();
        CStr modificator = gprotinfo->m_modifications[0].m_propertyName;

        LOG_INFO("--- Weapon Gadget id%d #%d (1st mod: %s), Price: %d ---",
            gadget_id,
            gadget->m_slotNum,
            modificator.charPtr,
            gprice);


        for (auto modification : gprotinfo->m_modifications)
        {
            if (significant_modifiers.find(modification.m_propertyName.charPtr) != significant_modifiers.end())
            {
                return gadget->GetPrice(0);
            }
            else
            {
                continue;
            }
        }
        return 0;

    }

    uint32_t __fastcall GetSchwarz(ai::Vehicle* vehicle, void* _)
    {
        LOG_DEBUG("> GetSchwarz <");
        LOG_WARNING("----- %s -----", vehicle->name);
        CStr vname = vehicle->name;

        uint32_t new_schwarz = 0;
        uint32_t total_price_by_game = 0;
        
        float hp_coeff = vehicle->GetHealth() / vehicle->GetMaxHealth();

        float base_schwarz_part = 0.5f;
        float armor_schwarz_part = 1.0f - base_schwarz_part;

        float totalPrice = 0.0f;
        
        std::unordered_map<CStr, ai::VehiclePart *> weapons;
        std::unordered_set<CStr> equiped_weapon_types;

        for (const auto& [part_name, veh_part] : vehicle->m_vehicleParts) {
            LOG_INFO("--- %s ---", veh_part->m_modelname);

            float part_condition_price_coeff = 1.0f;
            float part_durability = veh_part->m_durability.m_value.m_value;
            float part_max_durability = veh_part->m_durability.m_maxValue.m_value;

            if (part_name.Equal("CHASSIS"))
            {
                part_condition_price_coeff = hp_coeff;
                LOG_DEBUG("Skipping dur check for chassis");
            }
            else if (part_max_durability >= 0.001)
            {
                if (part_name.In({"CABIN", "BASKET"}))
                {
                    part_condition_price_coeff = part_durability / part_max_durability;
                    LOG_DEBUG("Dur: %.2f, MaxDur: %.2f, PriceCoeff: %.2f",
                        part_durability,
                        part_max_durability,
                        part_condition_price_coeff);
                }
                else // a gun
                {
                    weapons.insert({part_name, veh_part});
                    const ai::VehiclePartPrototypeInfo* gun_protinfo = veh_part->GetPrototypeInfo();
                    // equiped_weapon_types.emplace(gun_protinfo)
                    
                    part_condition_price_coeff = part_durability / part_max_durability;
                    if (part_condition_price_coeff >= 0.05) // let's try to only apply coeff to guns that are almost broken
                    {
                        part_condition_price_coeff = 1.0;
                    }
                    LOG_DEBUG("Dur: %.2f, MaxDur: %.2f, Gun PriceCoeff: %.2f",
                        part_durability,
                        part_max_durability,
                        part_condition_price_coeff);

                }
            }
            else
            {
                part_condition_price_coeff = 0.0;
                LOG_WARNING(
                    "Part '%s' has ~0 max durability and will not be correctly processed",
                    part_name.charPtr);
                continue;
            }

            float price_coeff = 1.0; // veh_part->GetPriceCoeff(0) will always == 1.0

            float part_price = veh_part->m_price.m_value;
            float schwarz_recalculated_float = price_coeff * part_condition_price_coeff * part_price;
            int schwarz_recalculated;
            if (schwarz_recalculated_float >= 1.0)
                schwarz_recalculated = (int)schwarz_recalculated_float;
            else
                schwarz_recalculated = 1;
            float price_by_game_actual = (float)veh_part->GetPrice(0);

            // TODO: check usage of GetPrice specific for Gun class (that also takes shells into account)
            LOG_DEBUG("Vanilla GetPrice of '%s' is %d (raw price is %.2f, recalculated schwarz is %d)",
                part_name.charPtr, // TODO: Plain said this is very naughty
                veh_part->GetPrice(0),
                part_price,
                schwarz_recalculated);

            total_price_by_game += veh_part->GetPrice(0);
            new_schwarz += schwarz_recalculated;
        }


        for (const auto& [gadget_id, gadget] : vehicle->m_gadgets){
            // gadget_id/slot 0 - 4 (COMMON), slot 5 - 9 (WEAPON)
            if (gadget_id <= GadgetId::GADGET_COMMON_MAX)
            {
                new_schwarz += GetCommonGadgetSchwarz(gadget, gadget_id, vehicle->m_vehicleParts);
            }
            else
            {
                new_schwarz += GetWeaponGadgetSchwarz(gadget, gadget_id, weapons);
            }
        }

        uint32_t vanilla_schwarz;
        uint32_t vanilla_base_price;

        float full_max_durability;
        float full_cur_durability;
 
        full_max_durability = vehicle->GetMaxFullDurability();
        full_cur_durability = vehicle->GetFullDurability();
        
        vanilla_base_price = vehicle->GetPrice(0);

        float full_dur_price_coeff;
        if (full_max_durability >= 0.001)
            full_dur_price_coeff = full_cur_durability / full_max_durability;
        else
            full_dur_price_coeff = 0.0;

        vanilla_schwarz = (uint32_t)(vanilla_base_price * full_dur_price_coeff);

        LOG_WARNING("----- New Schwarz: %d (vanilla total price: %d) -----",
            new_schwarz, total_price_by_game);
        LOG_WARNING("----- Vanilla Schwarz: %d, Base Price: %d, dur_coeff: %.2f (~%.2f/%.2f) -----",
            vanilla_schwarz, vanilla_base_price, full_dur_price_coeff, full_cur_durability, full_max_durability);
        return new_schwarz;
    }

    void Apply() {
        kraken::routines::Redirect(0x0088, (void*) 0x005E0DB0, (void*) &GetSchwarz);
    }
}
