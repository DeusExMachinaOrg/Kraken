#pragma once
#include "utils.hpp"
#include "hta/Func.h"
#include "stdafx.hpp"
#include "hta/CStr.h"
#include "hta/m3d/Landscape.hpp"
#include "hta/m3d/rend/TexHandle.hpp"
#include "hta/m3d/rend/VbHandle.hpp"
#include "hta/m3d/rend/IbHandle.hpp"

namespace m3d::cmn {
    struct XmlFile;
    struct XmlNode;
};

namespace m3d {
    struct Weather;
    struct CWorld;

    enum GlobalTimeParams {
        GTP_SUNRISE_TIME = 0,
        GTP_DAY_TIME = 1,
        GTP_SUNSET_TIME = 2,
        GTP_NIGHT_TIME = 3,
        GTP_NUM_PARAMS = 4
    };

    enum ColorItems : int32_t {
        CI_SKY = 0x0000,
        CI_FOG = 0x0001,
        CI_AMBIENT = 0x0002,
        CI_DIFFUSE = 0x0003,
        CI_SUN = 0x0004,
        CI_PLANT = 0x0005,
        CI_SPECULAR = 0x0006,
        CI_NUM_COLORITEMS = 0x0007,
    };

    class WeatherManager {
        /* Size=0x60 */
        /* 0x0000 */ stable_size_vector<Weather*> m_weatherStorage;
        /* 0x0010 */ stable_size_vector<Weather*> m_curWeatherStorage;
        /* 0x0020 */ Weather* m_currentWeather;
        /* 0x0024 */ float m_cloudsOffset;
        /* 0x0028 */ rend::TexHandle m_starsTexture;
        /* 0x002c */ CStr m_StarsTextureName;
        /* 0x0038 */ rend::TexHandle m_cloudTextureHandle;
        /* 0x003c */ GlobalTimeParams m_curDayTime;
        /* 0x0040 */ float m_globalTimeParams[4];
        /* 0x0050 */ bool m_bEdit;
        /* 0x0054 */ rend::VbHandle m_vbSky;
        /* 0x0058 */ rend::IbHandle m_ibSky;
        /* 0x005c */ CWorld* m_owner;
    
        WeatherManager(const WeatherManager&);
        WeatherManager();
        int32_t ReadFromXmlFile(const char*);
        int32_t WriteToXmlFile(const char*);
        int32_t SaveWeatherStateToXMLNode(cmn::XmlFile*, cmn::XmlNode*);
        int32_t LoadWeatherStateFromXMLNode(cmn::XmlFile*, cmn::XmlNode*);
        int32_t CreateSky();
        void DoneSky();
        int32_t SetupSkyParams();
        int32_t RenderWeather(Landscape::LandRenderMode);
        int32_t RenderWeatherParticles();
        int32_t UpdateDayTime();
        int32_t UpdateWheatherParticles();
        GlobalTimeParams GetCurrentDayTime() const;
        uint32_t GetWeatherColor(ColorItems) const;
        float GetFogReduceFactorFromWeather() const;
        float GetShadowTransparencyFromWeather() const;
        bool GetShadowVisibilityFromWeather() const;
        void SetActiveWeather(uint32_t);
        void SetActiveWeatherByName(const CStr&);
        const Weather* GetActiveWeather() const;
        Weather* GetWeather(uint32_t);
        Weather* GetWeatherByName(const CStr&);
        uint32_t GetNumWeathers() const;
        uint32_t GetGlobalTimeParamsNum() const;
        const char* GetGlobalTimeParamName(uint32_t) const;
        float GetGlobalTimeParam(uint32_t) const;
        float SetGlobalTimeParam(uint32_t, float);
        void SetEdit(bool);
        void ChangeStarsTexture(CStr&);
        const CStr& GetStarsTextureName() const;
        void AddWeather(const CStr&, const CStr&);
        int32_t DeleteWeather(uint32_t);
        void SetOwner(CWorld*);
        void ChangeCloudsTexture();
        void ChangeLightmapTexture();
        ~WeatherManager();
    };
    ASSERT_SIZE(WeatherManager, 0x60);
};