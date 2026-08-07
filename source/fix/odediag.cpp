#define LOGGER "odediag"

#include "ext/logger.hpp"
#include "fix/odediag.hpp"
#include "config.hpp"
#include "routines.hpp"

#include "hta/ai/DynamicScene.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/CVector.hpp"

namespace kraken::fix::odediag {
    // docs §46e (task #59): the user asked for a jolt=0 vs jolt=1 chassis-coordinate comparison
    // on the same save. fix::joltshadow's own diagnostics only ever install from inside its
    // Apply(), which returns immediately when [jolt]enabled=0 - useless for the jolt=0 half of
    // that comparison. Same pattern as fix::breakablediag (docs §60): a standalone module with
    // its own unconditional-of-jolt Apply(), hooking ai::DynamicScene::StepScene's call site
    // directly (VA 0x005F4260 - the SAME site fix::joltshadow::StepSceneHook patches when jolt
    // is on) so the exact same log line format is available whichever way the run was launched.
    //
    // Only ONE hook can own a given call site at a time (routines::ChangeCall overwrites the
    // call target, it doesn't chain), so this module's Apply() explicitly only installs when
    // [jolt]enabled=0 - fix::joltshadow's own Apply() already returns before installing its hook
    // in that exact case, so the two never contend for the same site.
    static void __fastcall StepSceneHook(hta::ai::DynamicScene* scene, void*, float elapsedTime) {
        scene->StepScene(elapsedTime);

        hta::ai::Vehicle* player = scene->GetVehicleControlledByPlayer();
        if (player != nullptr) {
            const hta::CVector odePivot = player->GetPosition();
            const hta::CVector odeCom   = player->GetMassCenterPosition();
            LOG_INFO("docs §46e: ODE chassis (player, jolt=0) GetPosition=(%.2f,%.2f,%.2f) "
                "GetMassCenterPosition=(%.2f,%.2f,%.2f)",
                (double) odePivot.x, (double) odePivot.y, (double) odePivot.z,
                (double) odeCom.x, (double) odeCom.y, (double) odeCom.z);
        }
    }

    void Apply() {
        const kraken::Config& config = kraken::Config::Instance();
        if (config.ode_diag.value == 0)
            return;
        if (config.jolt.value != 0) {
            // fix::joltshadow owns this call site when Jolt is on - its own diagnostics
            // (Shadow divergence) already cover this case, nothing to add here.
            LOG_INFO("docs §46e: [ode_diag]enabled=1 but [jolt]enabled=1 too - "
                "fix::joltshadow already logs chassis coordinates (Shadow divergence), skipping");
            return;
        }
        routines::ChangeCall((void*) 0x005F4260, &StepSceneHook);
        LOG_INFO("docs §46e: ODE chassis coordinate diagnostic installed (VA 0x005F4260), independent of [jolt] enabled");
    }
}
