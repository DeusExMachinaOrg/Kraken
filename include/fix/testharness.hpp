#pragma once

namespace kraken::fix::testharness {
    bool IsEnabled();

    // Installs the M0 scripted-input/telemetry hooks.
    //
    // The harness is intentionally independent of Config: the multiplayer branch does
    // not carry the Jolt branch's testharness.* ConfigValue fields.  Runtime options are
    // read locally from the environment; see source/fix/testharness.cpp.
    void Apply();
}
