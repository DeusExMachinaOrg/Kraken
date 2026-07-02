#pragma once

namespace kraken::fix::gamepad {
    void Apply();

    // Re-apply the [gamepad] JOY_BUTTON_* -> impulse bindings from the current
    // (possibly just-switched) control profile. No-op if the bridge is disabled.
    void Reapply();
}
