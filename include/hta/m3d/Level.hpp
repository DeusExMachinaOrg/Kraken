#pragma once

#include "hta/CStr.h"
#include "hta/m3d/Object.h"

namespace m3d {
    struct m3d::Level : public m3d::Object {
        /* Size=0x300 */
        /* 0x0000: fields for m3d::Object */
        /* 0x0034 */ CStr m_weatherDetailName;
        /* 0x0040 */ CStr m_weatherType;
        /* 0x004c */ int32_t m_currentDayTime;
        /* 0x0050 */ CStr m_levelPath;
        /* 0x005c */ CStr m_levelName;
        /* 0x0068 */ CStr m_hfName;
        /* 0x0074 */ CStr m_waterName;
        /* 0x0080 */ CStr m_cameraMapName;
        /* 0x008c */ CStr m_colormapName;
        /* 0x0098 */ CStr m_cliffsetName;
        /* 0x00a4 */ CStr m_cliffmapName;
        /* 0x00b0 */ CStr m_roadsetName;
        /* 0x00bc */ CStr m_roadmapName;
        /* 0x00c8 */ CStr m_waypointsName;
        /* 0x00d4 */ CStr m_beachsetsName;
        /* 0x00e0 */ CStr m_shoreLineName;
        /* 0x00ec */ CStr m_normalMapName;
        /* 0x00f8 */ CStr m_cubemapName;
        /* 0x0104 */ CStr m_dsSrvName;
        /* 0x0110 */ CStr m_questStatesFileName;
        /* 0x011c */ CStr m_externalPathsFileName;
        /* 0x0128 */ CStr m_staticObstaclesFileName;
        /* 0x0134 */ CStr m_playerPassMapFileName;
        /* 0x0140 */ CStr m_prototypeFullNames;
        /* 0x014c */ CStr m_ObjectFullNames;
        /* 0x0158 */ CStr m_serversname;
        /* 0x0164 */ CStr m_staticServers;
        /* 0x0170 */ CStr m_passMapName;
        /* 0x017c */ int32_t m_passMapCellSize;
        /* 0x0180 */ CStr m_TriggersName;
        /* 0x018c */ CStr m_cinemaTriggersName;
        /* 0x0198 */ CStr m_dialogStrings;
        /* 0x01a4 */ int32_t land_size;
        /* 0x01a8 */ float m_minSafex;
        /* 0x01ac */ float m_minSafey;
        /* 0x01b0 */ float m_maxSafex;
        /* 0x01b4 */ float m_maxSafey;
        /* 0x01b8 */ int32_t m_skyType;
        /* 0x01bc */ CStr m_envMapsNames[12];
        /* 0x024c */ float m_skyScale[2];
        /* 0x0254 */ float m_skyScrollSpeed;
        /* 0x0258 */ float m_skyRotateSpeed;
        /* 0x025c */ int32_t m_skyCloudsEnable;
        /* 0x0260 */ float m_sunAzimuthSpeed;
        /* 0x0264 */ float m_sunAzimuth;
        /* 0x0268 */ float m_sunDayAscention;
        /* 0x026c */ float m_sunRiseAscention;
        /* 0x0270 */ float m_sunSetAscention;
        /* 0x0274 */ uint32_t m_lsFarColor;
        /* 0x0278 */ uint32_t m_olsFarColor;
        /* 0x027c */ uint32_t m_lsSkyColor;
        /* 0x0280 */ uint32_t m_olsSkyColor;
        /* 0x0284 */ float waterlevel;
        /* 0x0288 */ float m_baseWaterLevel;
        /* 0x028c */ float m_skyDomeDivider;
        /* 0x0290 */ uint32_t m_reflectionTint;
        /* 0x0294 */ uint32_t m_refractionTint;
        /* 0x0298 */ float m_waterAbsorptionRed;
        /* 0x029c */ float m_waterAbsorptionGreen;
        /* 0x02a0 */ float m_waterAbsorptionBlue;
        /* 0x02a4 */ CStr m_waterTexSmall;
        /* 0x02b0 */ CStr m_waterTexBig;
        /* 0x02bc */ int32_t demostartup;
        /* 0x02c0 */ CStr demofile;
        /* 0x02cc */ CStr m_tilesFileName;
        /* 0x02d8 */ float m_maxHeight;
        /* 0x02dc */ uint32_t m_modelAmbient;
        /* 0x02e0 */ uint32_t m_omodelAmbient;
        /* 0x02e4 */ uint32_t m_modelDiffuse;
        /* 0x02e8 */ uint32_t m_omodelDiffuse;
        /* 0x02ec */ uint32_t m_lsAmbient;
        /* 0x02f0 */ uint32_t m_olsAmbient;
        /* 0x02f4 */ uint32_t m_lsDiffuse;
        /* 0x02f8 */ uint32_t m_olsDiffuse;
        /* 0x02fc */ float m_lsTFactor;

        static m3d::Class m_classLevel;

        Level();
        Level(const m3d::Level&);
        virtual ~Level();
        virtual m3d::Object* Clone();
        virtual m3d::Class* GetClass() const;
        int32_t New(CCamera&, int32_t);
        int32_t Load(const CStr&, CCamera&, bool);
        int32_t Save(const CStr&, const CCamera&);
        CStr GetFullPathNameA(const CStr&);
        int32_t GetLandSize() const;
        const char* GetLevelName() const;

        static m3d::Object* CreateObject();
        static m3d::Class* GetBaseClass();
    };
};