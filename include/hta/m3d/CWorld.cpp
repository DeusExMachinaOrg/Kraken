#include "hta/m3d/CWorld.hpp"

namespace m3d {
    WheelTraceMgr& CWorld::GetWheelTracesMgr() {
        return m_wheelTracesMgr;
    };

    Landscape& CWorld::GetLandscape() {
        return this->m_landscape;
    };
};