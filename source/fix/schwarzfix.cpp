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

namespace kraken::fix::schwarzfix {

    int GetGadgetSchwarz(ai::Gadget* gadget, const int gadget_id)
    {
        int gbelong = gadget->m_belong;
        unsigned int gprice = gadget->GetPrice(0);
        const ai::GadgetPrototypeInfo* gprotinfo = gadget->GetPrototypeInfo();
        CStr modificator = gprotinfo->m_modifications[0].m_propertyName;

        std::set<CStr, bool> weapon_gadgets_modifiers;
        std::unordered_set<std::string_view> validStrings = {
            "Durability", "MaxDurability", // any vehicle part
            "MaxTorque", "MaxSpeed", // cabin
            "MaxHealth", // chassis
            "Damage", "FiringRate", "ReChargingTime", "Accuracy", "FiringRange", // guns
        };

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

        // int gadget_id = 0;
        LOG_INFO("--- Gadget #%d (%s: id%d), Price: %d ---",
            gadget->m_slotNum,
            modificator.charPtr,
            gadget_id,
            gprice);

        int schwarz = gprice;
        return schwarz;
    }

    uint32_t __fastcall GetSchwarz(ai::Vehicle* vehicle, void* _)
    {
        LOG_DEBUG("> GetSchwarz <");
        LOG_WARNING("----- %s -----", vehicle->name);
        CStr vname = vehicle->name;

        int new_schwarz = 0;
        int total_price_by_game = 0;
        
        float hp_coeff = vehicle->GetHealth() / vehicle->GetMaxHealth();

        float base_schwarz_part = 0.5f;
        float armor_schwarz_part = 1.0f - base_schwarz_part;

        float totalPrice = 0.0f;
        
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
                // if (part_name.Equal("CABIN") || part_name.Equal("BASKET"))
                if (part_name.In({"CABIN", "BASKET"}))
                {
                    part_condition_price_coeff = part_durability / part_max_durability;
                    LOG_DEBUG("Dur: %.2f, MaxDur: %.2f, PriceCoeff: %.2f",
                        part_durability,
                        part_max_durability,
                        part_condition_price_coeff);
                }
                else if (part_max_durability >= 0.001)
                {
                    // should always be guns? (or gadgets also here?)
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
                else
                    part_condition_price_coeff = 0.0;
            }
            else
            {
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
            int gprice = GetGadgetSchwarz(gadget, gadget_id);
            new_schwarz += gprice;
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
