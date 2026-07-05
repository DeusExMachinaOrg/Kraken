#ifndef KRAKEN_FIX_XINPUTRUMBLE
#define KRAKEN_FIX_XINPUTRUMBLE

namespace hta::ai { struct Vehicle; }

namespace kraken::fix::xinputrumble {
    // Rumble for an XInput pad (e.g. a DualSense bridged to a virtual Xbox 360
    // controller by DSX / Steam Input, where the native HID path is unavailable).
    // No-op unless [xinput] enabled. XInput is loaded dynamically at runtime.
    void Apply();

    // Re-read the [xinput] config after a control-profile switch (gains + enable).
    void Reapply();

    // Called once per frame from the controls hook while a live player vehicle
    // exists: derives rumble force from the vehicle's motion (impacts, rough
    // ground, taking damage) and drives the pad's two motors.
    void Update(hta::ai::Vehicle* vehicle);

    // Called when there is no live player vehicle (menus / death): stops the motors.
    void Idle();

    // True if any XInput controller (slots 0..3) is currently connected — i.e. a
    // DS4Windows / DSX / Steam Input virtual Xbox pad, or a real Xbox controller.
    // Used to prefer the XInput input mode even when the DualSense HID is also
    // openable (those bridges do not hold the HID exclusively). Loads XInput on
    // demand, independent of [xinput] enabled.
    bool AnyConnected();
}

#endif
