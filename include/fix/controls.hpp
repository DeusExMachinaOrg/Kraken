#pragma once

namespace kraken::fix::controls {
    void Apply();

    // Re-read the [wheel] config after a control-profile switch and, if the new
    // profile enables the wheel for the first time, install the hook now. Safe to
    // call repeatedly; the override gates on the live enable flag.
    void Reapply();

    // Latest raw value [-1..1] of the selected controller's axis (0..5), as seen on
    // the impulse bus (device-filtered, before deadzone/mapping). Used by the
    // control-profile UI to draw a live position bar next to each axis row. Returns
    // 0 for out-of-range indices.
    float AxisLive(int axis);
}
