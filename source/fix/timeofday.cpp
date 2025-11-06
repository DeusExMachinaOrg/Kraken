#define LOGGER "timeofday"

#include <unordered_map>
#include <unordered_set>
#include "routines.hpp"
#include "configstructs.hpp"
#include "ext/logger.hpp"

#include "fix/timeofday.hpp"

#include "hta/ai/DynamicScene.hpp"
#include "hta/m3d/WeatherManager.h"
#include "hta/m3d/CWorld.hpp"
#include "hta/m3d/Level.hpp"
 #include "hta/ai/ObjContainer.hpp"
//#include "hta/ai/CServer.hpp"

namespace kraken::fix::timeofday {
    static inline ai::DynamicScene*& gDynamicScene = *(ai::DynamicScene**)0x00A12958;


    bool enable_fix{false};

    void __fastcall UpdateSun(m3d::CWorld* world, void* _)
    {
        m3d::GlobalTimeParams m_curDayTime;
        float m_sunAscention = 0;

        m_curDayTime = world->m_weatherManager.m_curDayTime;
        if ( m_curDayTime == m3d::GlobalTimeParams::GTP_SUNRISE_TIME )
            m_sunAscention = world->m_level->m_sunRiseAscention;
        else if ( m_curDayTime == m3d::GlobalTimeParams::GTP_DAY_TIME )
            m_sunAscention = world->m_level->m_sunDayAscention;
        else if ( m_curDayTime == m3d::GlobalTimeParams::GTP_SUNSET_TIME )
            m_sunAscention = world->m_level->m_sunSetAscention;
           
        world->m_sunAscention = m_sunAscention;

        m3d::AIParam game_time;
        ai::ObjContainer::theObjects->GetGameTime(&game_time);
        m3d::AIParam game_time24;
        ai::ObjContainer::theObjects->Get24HourTime(&game_time24);
        ai::GameTime& gametimeai = ai::ObjContainer::theObjects->m_GameTime;

        stable_size_vector<int>& newTime = game_time.GetAsIdList();
        stable_size_vector<int>& newTime24 = game_time24.GetAsIdList();

        //game_time.Detach(); // done this way in m3d::WeatherManager::UpdateDayTime
        //game_time24.Detach();

        float sunAscensionRad = world->m_sunAscention * 0.017453292f;
        float sunAzimuthRad = world->m_sunAzimuth * 0.017453292f;

        world->m_sunDir.x = cos(sunAzimuthRad) * cos(sunAscensionRad) * 20000.0f;
        world->m_sunDir.z = sin(sunAzimuthRad) * cos(sunAscensionRad) * 20000.0f;
        world->m_sunDir.y = sin(sunAscensionRad) * 20000.0f;

        float sunDirMagnitude = sqrt(
            world->m_sunDir.x * world->m_sunDir.x +
            world->m_sunDir.y * world->m_sunDir.y +
            world->m_sunDir.z * world->m_sunDir.z);

        // Normalize the sun direction vector
        world->m_sunDir.x = world->m_sunDir.x / sunDirMagnitude;
        world->m_sunDir.y = world->m_sunDir.y / sunDirMagnitude;
        world->m_sunDir.z = world->m_sunDir.z / sunDirMagnitude;
    }

    void Apply() {
        const kraken::Config& config = kraken::Config::Get();

        LOG_INFO("Feature enabled");

        enable_fix = true;
        kraken::routines::Redirect(0x0025, (void*) 0x005C7760, (void*) &UpdateSun);
    }
}