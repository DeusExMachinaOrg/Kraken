#include "fix/recollectionfix.hpp"
#include "routines.hpp"

#include "hta/ai/CServer.hpp"
#include "hta/ai/GlobalProperties.hpp"
#include "hta/ai/VehicleRecollection.hpp"
#include "hta/ai/Vehicle.hpp"

namespace kraken::fix::recollection {
    void __fastcall Update(hta::ai::VehicleRecollection* self, void* _, float elapsedTime, uint32_t workTime) {
        const float currentTime = hta::ai::CServer::Instance()->m_pObjects->GetGameTimeDiff();
        const float removalThreshold = currentTime - (hta::ai::GlobalProperties::Instance()->m_gameTimeMult * 3.0f);

        if (!self->m_recollectionItems.empty() && self->m_recollectionItems.back().time > currentTime) {
            self->m_recollectionItems.clear();
        }

        // Remove old entries that exceed the time threshold
        while (!self->m_recollectionItems.empty()) {
            const auto& oldestItem = self->m_recollectionItems.front();
            if (removalThreshold <= oldestItem.time)
                break;

            // Remove the oldest item
            self->m_recollectionItems.erase(self->m_recollectionItems.begin());
        }

        // Check if we should add a new recollection entry
        const bool shouldAddNewEntry = self->m_recollectionItems.empty() || (currentTime > (self->m_recollectionItems.back().time + hta::ai::GlobalProperties::Instance()->m_gameTimeMult * 0.3f));

        if (shouldAddNewEntry) {
            if (self->m_vehicleId >= 0) {
                // Get the object record for the vehicle
                auto* vehicleObj = reinterpret_cast<hta::ai::Vehicle*>(hta::ai::CServer::Instance()->m_pObjects->GetEntityByObjId(self->m_vehicleId));
                if (vehicleObj != nullptr) {
                    // Create new recollection item with current position
                    hta::ai::VehicleRecollection::ReollectionItem newItem;
                    newItem.pos = vehicleObj->GetGeometricCenter();
                    newItem.time = currentTime;
                    self->m_recollectionItems.push_back(newItem);
                    return;
                }
            }

            // If we get here, the vehicle is no longer valid - remove this recollection
            self->Remove();
        }
    }

    void Apply(void) {
        //kraken::routines::OverrideValue(reinterpret_cast<void*>(0x009ACB40), &hta::ai::VehicleRecollection::Update);
        kraken::routines::OverrideValue(reinterpret_cast<void*>(0x007D6950), &Update); // ??
    };
};
