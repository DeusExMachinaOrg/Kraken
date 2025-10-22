#define LOGGER "tactics"

#include "ext/logger.hpp"
#include "routines.hpp"
#include "config.hpp"

#include "hta/ai/Team.h"
#include "hta/m3d/AIParam.hpp"

#include "fix/tactics.hpp"

namespace kraken::fix::tactics {
    void __fastcall Fixed_AttackNow(ai::Team* team, int, int id)
    {
        ai::AI* pAI = &team->m_AI;

        const CStr& pRawName = pAI->GetCurState2Name();
        if (!pRawName.m_charPtr)
            return;

        if (pRawName.Equal("Attack"))
            return;

        team->_AdjustRoles(id);

        m3d::AIParam Param1 = {};
        m3d::AIParam Param2 = {};
        m3d::AIParam Param3 = {};

        Param1.Type = m3d::eAIParamType::AIPARAM_ID;
        Param1.id = id;

        pAI->InsCommand(2, Param1, Param2, Param3);

        Param2.Detach();
        Param3.Detach();
        Param1.Detach();
    }

    void Apply()
    {
        const kraken::Config& config = kraken::Config::Get();
        if (config.tactics.value == 0)
            return;

        LOG_INFO("Feature enabled");
        kraken::routines::Redirect(0xBF, (void*) 0x00658B50, Fixed_AttackNow);
    }
};
