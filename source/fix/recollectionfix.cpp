#include "fix/recollectionfix.hpp"
#include "routines.hpp"

#include "hta/ai/CServer.hpp"
#include "hta/ai/GlobalProperties.hpp"
#include "hta/ai/VehicleRecollection.hpp"

namespace kraken::fix::recollection {
    void __fastcall Update(hta::ai::VehicleRecollection* self, void* _, float elapsedTime, uint32_t workTime) {
        const float currentTime = hta::ai::CServer::Instance()->m_pObjects->GetGameTimeDiff();
        const float removalThreshold = currentTime - (hta::ai::GlobalProperties::theGlobProp->m_gameTimeMult * 3.0f);

        if (!self->m_recollectionItems.empty() && self->m_recollectionItems.back().time > currentTime) {
            m_recollectionItems.clear();
        }

        // Remove old entries that exceed the time threshold
        while (!self->m_recollectionItems.empty()) {
            const auto& oldestItem = self->m_recollectionItems.front();
            if (removalThreshold <= oldestItem.time)
                break;

            std::uint32_t it1 = 0;
            vector_reollection_begin(&m_recollectionItems, &it1);

            std::uint32_t it2 = 0;
            // Remove the oldest item
            vector_reollection_erase(&m_recollectionItems, &it2, it1);
        }

        // Check if we should add a new recollection entry
        const bool shouldAddNewEntry = self->m_recollectionItems.empty() || (currentTime > (self->m_recollectionItems.back().time + GlobalProperties::theGlobProp->m_gameTimeMult * 0.3f));

        if (shouldAddNewEntry) {
            if (m_vehicleId >= 0) {
                // Get the object record for the vehicle
                auto* vehicleObj = reinterpret_cast<Vehicle*>(CServer::Instance()->m_pObjects->GetEntityByObjId(m_vehicleId));
                if (vehicleObj != nullptr) {
                    // Create new recollection item with current position
                    ReollectionItem newItem;
                    newItem.pos = vehicleObj->GetGeometricCenter();
                    newItem.time = currentTime;
                    vector_reollection_push_back(&m_recollectionItems, &newItem);
                    return;
                }
            }

            // If we get here, the vehicle is no longer valid - remove this recollection
            Remove();
        }
    }

    void Apply(void) {
        kraken::routines::OverrideValue(reinterpret_cast<void*>(0x009ACB40), &ai::VehicleRecollection::Update);
    };
};
