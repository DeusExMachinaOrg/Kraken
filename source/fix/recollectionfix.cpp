#include "fix/recollectionfix.hpp"
#include "routines.hpp"

#include "hta/ai/CServer.hpp"
#include "hta/ai/GlobalProperties.hpp"
#include "hta/ai/VehicleRecollection.hpp"
#include "hta/ai/Vehicle.hpp"

namespace kraken::fix::recollection {
    class VehicleRecollection_Fixed {
      public:
        VehicleRecollection_Fixed() = delete;

        void Update(float, uint32_t) {
            const float currentTime = hta::ai::CServer::Instance()->m_pObjects->GetGameTimeDiff();
            const float removalThreshold = currentTime - (hta::ai::GlobalProperties::Instance()->m_gameTimeMult * 3.0f);

            if (!m_recollection.m_recollectionItems.empty() && m_recollection.m_recollectionItems.back().time > currentTime) {
                m_recollection.m_recollectionItems.clear();
            }

            // Remove old entries that exceed the time threshold
            while (!m_recollection.m_recollectionItems.empty()) {
                const auto& oldestItem = m_recollection.m_recollectionItems.front();
                if (removalThreshold <= oldestItem.time)
                    break;

                // Remove the oldest item
                m_recollection.m_recollectionItems.erase(m_recollection.m_recollectionItems.begin());
            }

            // Check if we should add a new recollection entry
            const bool shouldAddNewEntry = m_recollection.m_recollectionItems.empty() || (currentTime > (m_recollection.m_recollectionItems.back().time + hta::ai::GlobalProperties::Instance()->m_gameTimeMult * 0.3f));

            if (shouldAddNewEntry) {
                if (m_recollection.m_vehicleId >= 0) {
                    // Get the object record for the vehicle
                    auto* vehicleObj = reinterpret_cast<hta::ai::Vehicle*>(hta::ai::CServer::Instance()->m_pObjects->GetEntityByObjId(m_recollection.m_vehicleId));
                    if (vehicleObj != nullptr) {
                        // Create new recollection item with current position
                        hta::ai::VehicleRecollection::ReollectionItem newItem;
                        newItem.pos = vehicleObj->GetGeometricCenter();
                        newItem.time = currentTime;
                        m_recollection.m_recollectionItems.push_back(newItem);
                        return;
                    }
                }

                // If we get here, the vehicle is no longer valid - remove this recollection
                m_recollection.Remove();
            }
        }

      private:
        hta::ai::VehicleRecollection m_recollection;
    };

    void Apply() {
        routines::OverrideValue(reinterpret_cast<void*>(0x009ACB40), &VehicleRecollection_Fixed::Update);
    };
};
