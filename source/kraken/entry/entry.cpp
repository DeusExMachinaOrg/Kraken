#define LOGGER "ENTRY"

#include "common/stdafx.hpp"
#include "common/routines.hpp"
#include "config/config.hpp"

#include "logger/logger.hpp"
#include "runtime/runtime.hpp"
#include "impulse/impulse.hpp"

#include "fixes/fileserver.hpp"
#include "fixes/physic.hpp"
#include "fixes/autobrakefix.hpp"
#include "fixes/objcontupgrade.hpp"
#include "fixes/luabinds.hpp"
#include "fixes/posteffectreload.hpp"
#include "fixes/wareuse.hpp"
#include "fixes/recollectionfix.hpp"
#include "fixes/ultrawide.hpp"
#include "fixes/fastloading.hpp"
#include "fixes/kineticfriction.hpp"
#include "fixes/cardan.hpp"
#include "fixes/tactics.hpp"
#include "fixes/complexschwarz.hpp"
#include "fixes/skinfix.hpp"
#include "fixes/cctlleakfix.hpp"
#include "fixes/locationdebug.hpp"
#include "fixes/difficultywndescapefix.hpp"
#include "fixes/mortarvolleylauncherfix.hpp"
#include "fixes/gunlights.hpp"

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
        routines::Override(sizeof(float),    (void*) 0x00602D4E, (char*) &G_CONFIG->contact_surface_layer.value);
        routines::Override(sizeof(float),    (void*) 0x00602D5E, (char*) &G_CONFIG->cfm.value);
        routines::Override(sizeof(float),    (void*) 0x00602D6E, (char*) &G_CONFIG->erp.value);
        routines::OverrideValue((void*) 0x0056BF09, (uint8_t) 0xEB); // Render all quest icons on radar

        // TODO: [Invesigation] Repaint Price
        // That's not work. Need to more deep research for fix it.
        // Look here [0x00474312] void __thiscall SkinsWnd::BuySkin(SkinsWnd *this)
        // routines::Override(sizeof(uint32_t), (void*) 0x00474641, (char*) &G_CONFIG->price_paint.value);
    };

    API void EntryPoint(HANDLE module) {
        G_MODULE = module;

        logger::Init();
        runtime::Init();
        impulse::Init();

        LOG_INFO("Prepare patches");
        ConstantHotfix();
        fix::fileserver::Apply();
        fix::physic::Apply();
        fix::autobrakefix::Apply();
        fix::objcontupgrade::Apply();
        fix::luabinds::Apply(G_CONFIG);
        fix::posteffectreload::Apply(G_CONFIG);
        fix::wareuse::Apply();
        fix::recollection::Apply();
        fix::ultrawide::Apply();
        fix::fastloading::Apply();
        //fix::kineticfriction::Apply();
        fix::cardan::Apply();
        fix::tactics::Apply();
        fix::complexschwarz::Apply();
        fix::skinfix::Apply();
        fix::cctlleakfix::Apply();
        fix::locationdebug::Apply();
        fix::difficultywndescapefix::Apply();
        fix::mortarvolleylauncherfix::Apply();
        fix::gunlights::Apply();
    };
};