#pragma once

namespace kraken::fix::controls {
    void Apply();

    // Re-read the [wheel] config after a control-profile switch and, if the new
    // profile enables the wheel for the first time, install the hook now. Safe to
    // call repeatedly; the override gates on the live enable flag.
    void Reapply();
}
