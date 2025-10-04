#include "stdafx.hpp"
#include "config.hpp"
#include "routines.hpp"

#include "ext/runtime.hpp"

#include "fix/fileserver.hpp"
#include "fix/physic.hpp"
#include "fix/autobrakefix.hpp"
#include "fix/objcontupgrade.hpp"
#include "hta/pointers.hpp"

namespace kraken {
    HANDLE  G_MODULE = nullptr;
    Config* G_CONFIG = new Config();

    void ConstantHotfix() {
        routines::Override(sizeof(uint32_t), (void*) 0x0057BCAF, (char*) &G_CONFIG->save_height.value);
        routines::Override(sizeof(uint32_t), (void*) 0x0070808B, (char*) &G_CONFIG->view_resolution.value);
        routines::Override(sizeof(uint32_t), (void*) 0x00708092, (char*) &G_CONFIG->view_resolution.value);
        routines::Override(sizeof(float),    (void*) 0x00602D25, (char*) &G_CONFIG->gravity.value);
        routines::Override(sizeof(uint32_t), (void*) 0x005539D5, (char*) &G_CONFIG->price_fuel.value);
        routines::Override(sizeof(uint32_t), (void*) 0x0057BCA8, (char*) &G_CONFIG->save_width.value);
        routines::RemapPtr((void*) 0x005DAC06, &G_CONFIG->keep_throttle.value);
        routines::RemapPtr((void*) 0x005DAC81, &G_CONFIG->handbrake_power.value);
        routines::Override(sizeof(float), (void*) 0x004017DB, (char*) &G_CONFIG->brake_power.value);
        routines::Override(sizeof(bool),  (void*) 0x007DFADC, (char*) &G_CONFIG->friend_damage.value);

        // TODO: [Invesigation] Repaint Price
        // That's not work. Need to more deep research for fix it.
        // Look here [0x00474312] void __thiscall SkinsWnd::BuySkin(SkinsWnd *this)
        // routines::Override(sizeof(uint32_t), (void*) 0x00474641, (char*) &G_CONFIG->price_paint.value);
    };

    API void EntryPoint(HANDLE module) {
        G_MODULE = module;

        runtime::Init();

        ConstantHotfix();
        fix::fileserver::Apply();
        fix::physic::Apply();
        fix::autobrakefix::Apply();
        fix::objcontupgrade::Apply();
    };
};