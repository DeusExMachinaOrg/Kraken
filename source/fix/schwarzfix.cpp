#define LOGGER "SCHWARZ"

#include "hta/ai/Vehicle.h"
#include "hta/ai/VehiclePart.h"
#include "hta/ai/ComplexPhysicObj.h"
#include "fix/schwarzfix.hpp"
#include "routines.hpp"
#include "ext/logger.hpp"

namespace kraken::fix::schwarzfix {

    uint32_t __fastcall GetSchwarz(ai::Vehicle* vehicle, void* _)
    {
        LOG_DEBUG("----- %s -----", vehicle->name);

        int new_schwarz = 0;
        int total_price_by_game = 0;
        uint32_t improved_schwarz;
        
        float hp_coeff = vehicle->GetHealth() / vehicle->GetMaxHealth();

        float max_durability;
        float cur_durability;
        
        float durability_coeff;

        float base_schwarz_part = 0.5f;
        float armor_schwarz_part = 1.0f - base_schwarz_part;
        
        max_durability = vehicle->GetMaxFullDurability();
        cur_durability = vehicle->GetFullDurability();
        durability_coeff = cur_durability / max_durability * 100;
        
        float totalPrice = 0.0f;
        
        for (const auto& [part_name, veh_part] : vehicle->m_vehicleParts) {
            LOG_DEBUG("--- %s ---", veh_part->m_modelname);

            float dur_price_coeff;
            if (part_name.Equal("CHASSIS"))
            {
                dur_price_coeff = hp_coeff;
                LOG_DEBUG("Skipping dur check for chassis");
            }
            else
            {
                float part_durability = veh_part->m_durability.m_value.m_value;
                float part_max_durability = veh_part->m_durability.m_maxValue.m_value;
                if (part_max_durability >= 0.001)
                    dur_price_coeff = part_durability / part_max_durability;
                else
                    dur_price_coeff = 0.0;
                LOG_DEBUG("Dur: %.2f, MaxDur: %.2f, PriceCoeff: %.2f",
                    part_durability,
                    part_max_durability,
                    dur_price_coeff);
            }

            float price_coeff = 1.0; // veh_part->GetPriceCoeff(0) will always == 1.0

            float part_price = veh_part->m_price.m_value;
            float price_by_game_calc = price_coeff * dur_price_coeff * part_price;
            int recalculated_price;
            if (price_by_game_calc >= 1.0)
                recalculated_price = (int)price_by_game_calc;
            else
                recalculated_price = 1;
            float price_by_game_actual = veh_part->GetPrice(0);

            LOG_DEBUG("Vanilla GetPrice of '%s' is %d (raw price is %.2f, recalculated Price is %d)",
                part_name.charPtr, // TODO: Plain said this is very naughty
                veh_part->GetPrice(0),
                part_price,
                recalculated_price);

            total_price_by_game += veh_part->GetPrice(0);
            new_schwarz += recalculated_price;
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

        vanilla_schwarz = vanilla_base_price * full_dur_price_coeff;

        LOG_DEBUG("----- New Schwarz: %d (vanilla total price: %d) -----",
            new_schwarz, total_price_by_game);
        LOG_DEBUG("----- Vanilla Schwarz: %d, Base Price: %d, dur_coeff: %.2f (~%.2f/%.2f) -----",
            vanilla_schwarz, vanilla_base_price, full_dur_price_coeff, full_cur_durability, full_max_durability);
        return new_schwarz;
    }

    void Apply() {
        kraken::routines::Redirect(0x0088, (void*) 0x005E0DB0, (void*) &GetSchwarz);
    }
}
