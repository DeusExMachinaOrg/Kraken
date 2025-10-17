#pragma once
#include "CWorld.h"

namespace m3d {
    struct IConHandler {
        void* __vftable;
    };
    class Level;
    class Profiler;
}

namespace ai {
    class Map;
    class DynamicScene;
    class ObjContainer;
    class AIManager;
    class AffixManager;
    class ExternalPaths;
    class PlayerPassMap;
}

namespace ai {
    struct CServer : m3d::IConHandler // sizeof=0x98
    {
        inline static CServer*& Instance = *(CServer**)0x00A11BB4;
        ai::Map* pGlobalMap;
        bool m_StartServerUpdates;
        // padding byte
        // padding byte
        // padding byte
        m3d::CWorld *m_pWorld;
        m3d::Level *m_level;
        ai::DynamicScene *m_pDynamicScene;
        ai::ObjContainer *m_pObjects;
        ai::AIManager *pAIManager;
        ai::AffixManager *m_pAffixManager;
        ai::ExternalPaths *m_pExternalPaths;
        ai::PlayerPassMap *m_pPlayerPassMap;
        float m_LastUpdateTime;
        CVector cam;
        bool PrevState;
        bool fPause;
        // padding byte
        // padding byte
        int m_lastId;
        float m_averageElapsedTime;
        std::vector<float> m_lastElapsedTimes;
        int m_CurIndex;
        int m_MaxAverageLength;
        bool m_Accumulation;
        bool m_AveElapsedTimeUsed;
        bool m_InCinematic;
        // padding byte
        int m_LastSenderID;
        m3d::Profiler *m_profilerTmpForServer;
        m3d::Profiler *m_collideProfiler;
        m3d::Profiler *m_objectsUpdateProfiler;
        m3d::Profiler *m_serverUpdateProfiler;
        m3d::Profiler *m_pathFindingProfiler;
        m3d::Profiler *m_bulletProfiler;
        m3d::Profiler *m_blastWaveProfiler;
        stable_size_queue<int> m_consoleCommandsToPostProcess;
    };
}