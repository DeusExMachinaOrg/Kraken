#include "hta/ai/CServer.hpp"

namespace ai {
    CServer* CServer::Instance() {
        return *(CServer**)(0x00A11BB4);
    };

    m3d::CWorld* CServer::GetWorld() {
        return this->m_pWorld;
    };
};