#include "hta/ai/Vehicle.h"
#include "hta/ai/VehiclePart.h"
#include "hta/ai/ComplexPhysicObj.h"
#include "fix/schwarzfix.hpp"
#include "routines.hpp"

namespace kraken::fix::schwarzfix {

    uint32_t __fastcall GetSchwarz(ai::Vehicle* vehicle, void* _)
    {
        uint32_t price;

        uint32_t base_price;
        float max_durability;
        float cur_durability;
        
        float durability_coeff;

        float base_schwarz_part = 0.5f;
        float armor_schwarz_part = 1.0f - base_schwarz_part;
        
        max_durability = vehicle->GetMaxFullDurability();
        cur_durability = vehicle->GetFullDurability();
        durability_coeff = cur_durability / max_durability * 100;
        
        base_price = vehicle->GetPrice(0);

        price = base_price; // + (base_price * armor_schwarz_part * durability_coeff);

        float totalPrice = 0.0f;
        
        for (const auto& [part_name, part] : vehicle->m_vehicleParts) {
            totalPrice += part->GetPrice(0);
        }

        return totalPrice;
    }

    void Apply() {
        kraken::routines::Redirect(0x0088, (void*) 0x005E0DB0, (void*) &GetSchwarz);
    }
}