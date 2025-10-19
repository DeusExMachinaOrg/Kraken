#pragma once

#include "hta/CVector.h"
#include "hta/m3d/rend/TexHandle.hpp"

#include <stdint.h>


namespace m3d {
    enum FlareMode : int32_t {
        FLARE_SUN = 0x0001,
        FLARE_FLASH_NO_TEST = 0x0002,
        FLARE_TEST = 0x0004,
        FLARE_BLINDING = 0x0008,
        FLARE_SUN_FLASH = 0x000e,
    };

    struct CFlare {
        /* Size=0x18 */
        /* 0x0000 */ float m_ssx;
        /* 0x0004 */ float m_ssy;
        /* 0x0008 */ bool m_onScreen;
        /* 0x000c */ float m_percentageVisible;
        /* 0x0010 */ uint32_t m_event;
        /* 0x0014 */ int32_t m_glowOnScreen;

        static rend::TexHandle m_tex[6];
        static rend::TexHandle m_texSunGlow;

        CFlare();
        ~CFlare();
        int32_t Render(FlareMode, const CVector&, float, float);

        static int32_t Init();
        static void Release();
    };
};