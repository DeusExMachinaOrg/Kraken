#pragma once

#include "utils.hpp"
#include "hta/CVector.h"
#include "hta/m3d/CWorld.hpp"
#include "hta/m3d/IConHandler.hpp"
#include "hta/m3d/Profiler.hpp"
#include "hta/ai/Obj.hpp"
#include "hta/ai/ObjContainer.hpp"

#include <queue>

namespace m3d::cmn {
    struct XmlFile;
    struct XmlNode;
}

namespace m3d {
    struct CWorld;
    struct Level;
}

namespace ai {
    struct Map;
    struct AIManager;
    struct DynamicScene;
    struct AffixManager;
    struct ExternalPaths;
    struct PlayerPassMap;

    enum StartupMode : int32_t {
        LOCAL_GAME = 0x0000,
        SERVER_GAME = 0x0001,
        CLIENT_GAME = 0x0002,
    };

    enum eTolerance : int32_t {
        RS_ENEMY = 0x0001,
        RS_NEUTRAL = 0x0002,
        RS_ALLY = 0x0003,
        RS_OWN = 0x0004,
        RS_MAX = 0x0005,
    };

    struct CServer : public m3d::IConHandler {
        /* Size=0x98 */
        /* 0x0000: fields for m3d::IConHandler */
        /* 0x0004 */ Map* pGlobalMap;
        /* 0x0008 */ bool m_StartServerUpdates;
        /* 0x000c */ m3d::CWorld* m_pWorld;
        /* 0x0010 */ m3d::Level* m_level;
        /* 0x0014 */ DynamicScene* m_pDynamicScene;
        /* 0x0018 */ ObjContainer* m_pObjects;
        /* 0x001c */ AIManager* pAIManager;
        /* 0x0020 */ AffixManager* m_pAffixManager;
        /* 0x0024 */ ExternalPaths* m_pExternalPaths;
        /* 0x0028 */ PlayerPassMap* m_pPlayerPassMap;
        /* 0x002c */ float m_LastUpdateTime;
        /* 0x0030 */ CVector cam;
        /* 0x003c */ bool PrevState;
        /* 0x003d */ bool fPause;
        /* 0x0040 */ int32_t m_lastId;
        /* 0x0044 */ float m_averageElapsedTime;
        /* 0x0048 */ stable_size_vector<float> m_lastElapsedTimes;
        /* 0x0058 */ int32_t m_CurIndex;
        /* 0x005c */ int32_t m_MaxAverageLength;
        /* 0x0060 */ bool m_Accumulation;
        /* 0x0061 */ bool m_AveElapsedTimeUsed;
        /* 0x0062 */ bool m_InCinematic;
        /* 0x0064 */ int32_t m_LastSenderID;
        /* 0x0068 */ m3d::Profiler* m_profilerTmpForServer;
        /* 0x006c */ m3d::Profiler* m_collideProfiler;
        /* 0x0070 */ m3d::Profiler* m_objectsUpdateProfiler;
        /* 0x0074 */ m3d::Profiler* m_serverUpdateProfiler;
        /* 0x0078 */ m3d::Profiler* m_pathFindingProfiler;
        /* 0x007c */ m3d::Profiler* m_bulletProfiler;
        /* 0x0080 */ m3d::Profiler* m_blastWaveProfiler;
        /* 0x0084 */ stable_size_deque<int> m_consoleCommandsToPostProcess;
    
        CServer(const CServer&);
        CServer();
        virtual ~CServer();
        bool GetPause() const;
        void SetPause(bool);
        int32_t GetLastId();
        void SetLastId(int32_t);
        virtual void HandleCommand(int32_t, const m3d::CConsoleParams&);
        virtual bool HandleCVar(const m3d::CVar*, const m3d::CConsoleParams&);
        void Init(m3d::CWorld*);
        void Clear();
        void InitOnce();
        void ClearOnce();
        void Load(StartupMode, m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*, bool, ObjContainer::eSAVE_TYPES);
        void LoadVisitedMap(const CStr&, bool);
        void SaveVisitedMap(const CStr&);
        void Update(float);
        void GetControlData();
        void PutGameData();
        m3d::CWorld* GetWorld();
        int32_t GetPathFindQuant();
        float GetMaxWorkTime();
        CStr GetMaxWorkTimeClassNum();
        void RelinkSceneGraphNodes();
        int32_t GetPrototypeId(const CStr&);
        CStr GetFullNameByObjID(int32_t);
        eTolerance CheckTolerance(int32_t, int32_t);
        const PrototypeInfo* GetPrototypeInfo(int32_t);
        CStr GetPrototypeFullName(int32_t);
        CStr GetPrototypeFullName(const CStr&);
        void PostPlayerEvent(eGameEvent);
        uint32_t GetGlobalMapValue(int32_t, int32_t);
        void LoadGlobalMapFromRawFile(const char*);
        void LoadGlobalPropertiesFromXML(const CStr&);
        void LoadPrototypeNamesFromXML(const CStr&);
        void LoadTriggersFromXML(const CStr&);
        AffixManager* GetAffixManager() const;
        const ExternalPaths* GetExternalPaths() const;
        ExternalPaths* GetExternalPathsUnsafe();
        const PlayerPassMap* GetPlayerPassMap() const;
        void StartCinematic();
        void EndCinematic();
        void ResetCinematicObjects();
        void AddToCinematic(Obj*, bool);
        void AddToCinematic(int32_t, bool);
        bool IsInCinematic();
        m3d::Level* GetLevel() const;
        float GetLevelSize() const;
        m3d::Profiler* GetTmpProfiler();
        m3d::Profiler* GetCollideProfiler();
        m3d::Profiler* GetObjectsUpdateProfiler();
        m3d::Profiler* GetPathFindingProfiler();
        m3d::Profiler* GetBulletProfiler();
        m3d::Profiler* GetBlastWaveProfiler();
        PrototypeInfo* CreatePrototypeInfoByClassName(const CStr&);
        void _RegisterConsoleCommands();
        void _PostProcessConsoleCommands();
        void _SetLevel(m3d::Level*);
        void SavePrevCinematicState();
        void RestorePrevCinematicState();

        static void Register();
        static CServer* Instance();
    };
    ASSERT_SIZE(CServer, 0x98);
};