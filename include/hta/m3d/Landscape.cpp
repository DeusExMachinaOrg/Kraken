#include "hta/m3d/Landscape.hpp"
#include "hta/m3d/CWorld.hpp"
#include "hta/m3d/Level.hpp"

namespace m3d {
    int32_t Landscape::GetTileSize() const {
        return 4 * this->m_owner->m_level->land_size;
    };
};