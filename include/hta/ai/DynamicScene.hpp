#pragma once
#include "Obj.h"
#include "containers.hpp"
#include "Vehicle.h"

namespace ai {
    struct DynamicScene : m3d::Object {
        struct SoilProps {
            /* Size=0x28 */
            /* 0x0000 */ int16_t m_splashType;
            /* 0x0004 */ CStr m_splashTypeName;
            /* 0x0010 */ CStr m_wheelTraceTextureName;
            /* 0x001c */ float m_friction;
            /* 0x0020 */ float m_resistance;
            /* 0x0024 */ int32_t m_idx;

            SoilProps(const SoilProps&);
            SoilProps();
            void LoadFromXml(const m3d::cmn::XmlNode*);
            ~SoilProps();
        };
        static_assert(sizeof(SoilProps) == 0x28);

        /* Size=0x160 */
        /* 0x0000: fields for m3d::Object */
        /* 0x0034 */ hta::vector<SoilProps> m_soilProps;
        /* 0x0044 */ hta::vector<hta::vector<unsigned short>> m_soilPropsIdx;
        /* 0x0054 */ hta::vector<CStr> m_wheelTypeNames;
        /* 0x0064 */ hta::vector<hta::vector<CStr>> m_soilEffectNames;
        /* 0x0074 */ hta::vector<CStr> m_roadEffectNames;
        /* 0x0084 */ hta::vector<CStr> m_soilSplashTypeNames;
        /* 0x0094 */ hta::vector<CStr> m_shellTypesNames;
        /* 0x00a4 */ hta::vector<hta::vector<CStr>> m_shellsEffectsNames;
        /* 0x00b4 */ hta::vector<CStr> m_shellWaterEffectNames;
        /* 0x00c4 */ hta::vector<CStr> m_shellsStaticsEffNames;
        /* 0x00d4 */ hta::vector<CStr> m_shellsVehiclesEffNames;
        /* 0x00e4 */ hta::vector<CStr> m_shellsRoadEffNames;
        /* 0x00f4 */ hta::vector<CStr> m_vehicleSoilEffectNames;
        /* 0x0104 */ hta::vector<CStr> m_BoEffectTypeNames;
        /* 0x0114 */ hta::vector<CStr> m_BoVehicleEffectNames;
        /* 0x0124 */ hta::vector<hta::vector<CStr>> m_BoShellEffectNames;
        /* 0x0134 */ hta::vector<CStr> m_decalsNames;
        /* 0x0144 */ int32_t m_clashDecalId;
        /* 0x0148 */ float m_physicTimeAccumulator;
        /* 0x014c */ hta::deque<int> m_timefilterValues;

        Vehicle* GetVehicleControlledByPlayer() const
		{
			FUNC(0x005C7920, Vehicle*, __thiscall, _GetVehicleControlledByPlayer, const DynamicScene*);
			return _GetVehicleControlledByPlayer(this);
		}

    };
}
