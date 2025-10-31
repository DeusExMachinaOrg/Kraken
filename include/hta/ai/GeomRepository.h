#pragma once
#include <utils.hpp>
#include <containers.hpp>
#include <hta/ai/GeomRepositoryItem.h>

namespace ai
{
	struct GeomRepository
	{
        enum SortStyle : int32_t {
            SORT_NONE = 0x0000,
            SORT_BY_RESOURCE = 0x0001,
        };
        /* Size=0x64 */
        /* 0x0000: fields for m3d::Object */
        /* 0x0034 */ bool m_Changed;
        /* 0x0038 */ PointBase<int> m_geomSize;
        /* 0x0040 */ hta::vector<GeomRepositoryItem> m_slots;
        /* 0x0050 */ hta::set<int> m_referenceChests;
        /* 0x005c */ int32_t m_vehicleId;
        /* 0x0060 */ SortStyle m_sortStyle;
        static m3d::Class m_classGeomRepository;
		
		void GiveUpThingByObjId(int objId)
		{
			FUNC(0x006CAFA0, void, __thiscall, _OnMouseButton0, GeomRepository*, int);
			_OnMouseButton0(this, objId);
		}
	};
}