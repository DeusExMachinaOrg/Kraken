#ifndef KRAKEN_FIX_DUALSENSE
#define KRAKEN_FIX_DUALSENSE

namespace hta::ai { struct Vehicle; }

namespace kraken::fix::dualsense {
    // Opens the DualSense over HID (Bluetooth report 0x31 / USB 0x02) and starts
    // the output worker. No-op unless [dualsense] enabled.
    void Apply();

    // Re-read the [dualsense] config after a control-profile switch (gains +
    // enable/disable). The HID device is still opened lazily in Update.
    void Reapply();

    // Called once per frame from the controls hook while a live player vehicle
    // exists: derives rumble and adaptive-trigger force (L2/R2 resistance, plus a
    // kick on impacts/damage and a buzz on rough ground) from the vehicle's motion
    // and publishes it to the worker thread.
    void Update(hta::ai::Vehicle* vehicle);

    // Called when there is no live player vehicle (menus / death): fades the
    // actuators out so the pad doesn't keep buzzing.
    void Idle();

    // True when Kraken currently has the DualSense HID open (native wireless/USB
    // mode — DSX/Steam not holding it exclusively). Lets the device picker offer
    // the controller and lets the profile pick the native axis layout on reset.
    bool NativePresent();
    // System product name of the open DualSense (HID product string, e.g.
    // "Wireless Controller"); "DualSense (HID)" if the string is unavailable.
    // Empty when not present.
    const char* NativeName();

    // Wireless path: read the DualSense input report (0x31) over HID and inject
    // axes/buttons into the impulse bus (bypassing winmm, which can't read the
    // controller once it switches to full-report mode). Called from the joystick
    // poll timer (message-pump thread), so dispatched events run on the right
    // thread. No-op unless [dualsense] hid_input is set.
    void PumpInput();
}

#endif
