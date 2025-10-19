#include "hta/ai/CServer.hpp"
#include "hta/m3d/Level.hpp"

namespace ai {
    CServer* CServer::Instance() {
        return *(CServer**)(0x00A11BB4);
    };

    m3d::CWorld* CServer::GetWorld() {
        return this->m_pWorld;
    };

    float CServer::GetLevelSize() const {
        return this->m_level->land_size;
    };
};