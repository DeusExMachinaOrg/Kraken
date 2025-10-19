#pragma once
#include "Vehicle.hpp"
#include "hta/ai/CServer.hpp"
#include "hta/ai/ObjContainer.hpp"
#include "GlobalProperties.h"

namespace m3d
{
    namespace cmn
    {
        struct XmlFile;
        struct XmlNode;
    }
}

namespace ai
{
	struct ReollectionItem
	{
	  /* 0x0004 */ CVector pos;
	  /* 0x0010 */ float time;

      ReollectionItem()
      {
          FUNC(0x007D50D0, void, __thiscall, _ReollectionItem, ReollectionItem*);
          _ReollectionItem(this);
      }

      virtual void LoadFromXML(m3d::cmn::XmlFile* xmlFile, m3d::cmn::XmlNode* xmlNode)
      {
          FUNC(0x007D5170, void, __thiscall, _LoadFromXML, ReollectionItem*, m3d::cmn::XmlFile*, m3d::cmn::XmlNode*);
          return _LoadFromXML(this, xmlFile, xmlNode);
      }

      virtual void SaveToXML(m3d::cmn::XmlFile* xmlFile, m3d::cmn::XmlNode* xmlNode)
      {
          FUNC(0x007D57F0, void, __thiscall, _SaveToXML, ReollectionItem*, m3d::cmn::XmlFile*, m3d::cmn::XmlNode*);
          return _SaveToXML(this, xmlFile, xmlNode);
      }
	}; /* size: 0x0014 */

    void __fastcall vector_reollection_push_back(stable_size_vector<ai::ReollectionItem>* vec, ai::ReollectionItem* value)
    {
        FUNC(0x007D68C0, void, __thiscall, _vector_reollection_push_back, stable_size_vector<ai::ReollectionItem>*, ai::ReollectionItem*);
        return _vector_reollection_push_back(vec, value);
    }

    void __fastcall vector_reollection_clear(stable_size_vector<ai::ReollectionItem>* vec)
    {
        FUNC(0x007D6350, void, __thiscall, _vector_reollection_clear, stable_size_vector<ai::ReollectionItem>*);
        return _vector_reollection_clear(vec);
    }

    std::uint32_t* __fastcall vector_reollection_erase(
        stable_size_vector<ai::ReollectionItem>* vec,
        std::uint32_t* result,
        std::uint32_t where)
    {
        FUNC(0x007D6320, std::uint32_t*, __thiscall, _vector_reollection_erase,
            stable_size_vector<ai::ReollectionItem>*, std::uint32_t*, std::uint32_t);
        return _vector_reollection_erase(vec, result, where);
    }

    std::uint32_t* __fastcall vector_reollection_begin(stable_size_vector<ai::ReollectionItem>* vec, std::uint32_t* result)
    {
        FUNC(0x007D5870, std::uint32_t*, __thiscall, _vector_reollection_begin, stable_size_vector<ai::ReollectionItem>*, std::uint32_t*);
        return _vector_reollection_begin(vec, result);
    }

	struct VehicleRecollection : public Obj
	{
        void Update(float elapsedTime, std::uint32_t workTime)
        {
            const float currentTime = CServer::Instance()->m_pObjects->GetGameTimeDiff();
            const float removalThreshold = currentTime - (GlobalProperties::theGlobProp->m_gameTimeMult * 3.0f);

            if (!m_recollectionItems.empty() && m_recollectionItems.back().time > currentTime)
            {
                vector_reollection_clear(&m_recollectionItems);
            }

            // Remove old entries that exceed the time threshold
            while (!m_recollectionItems.empty())
            {
                const auto& oldestItem = m_recollectionItems.front();
                if (removalThreshold <= oldestItem.time)
                    break;

                std::uint32_t it1 = 0;
                vector_reollection_begin(&m_recollectionItems, &it1);

                std::uint32_t it2 = 0;
                // Remove the oldest item
                vector_reollection_erase(&m_recollectionItems, &it2, it1);
            }

            // Check if we should add a new recollection entry
            const bool shouldAddNewEntry = m_recollectionItems.empty() ||
                (currentTime > (m_recollectionItems.back().time + GlobalProperties::theGlobProp->m_gameTimeMult * 0.3f));

            if (shouldAddNewEntry)
            {
                if (m_vehicleId >= 0)
                {
                    // Get the object record for the vehicle
                    auto* vehicleObj = reinterpret_cast<Vehicle*>(CServer::Instance()->m_pObjects->GetEntityByObjId(m_vehicleId));
                    if (vehicleObj != nullptr)
                    {
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
	/* 0x00c0 */ stable_size_vector<ReollectionItem> m_recollectionItems;
	/* 0x00d0 */ std::int32_t m_vehicleId;
	}; /* size: 0x00d4 */
}
ASSERT_SIZE(ai::VehicleRecollection, 0x00d4);
