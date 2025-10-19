#include "hta/m3d/RoadSet.hpp"
#include "hta/m3d/RoadNode.hpp"
#include "hta/m3d/RoadManager.hpp"

namespace m3d {
    int32_t RoadNode::GetSoilType() {
        return this->m_owner->m_roadSets[this->m_roadSetHandle]->m_soilType;
    };
};