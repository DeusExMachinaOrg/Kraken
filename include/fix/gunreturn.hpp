#ifndef KRAKEN_FIX_GUNRETURN
#define KRAKEN_FIX_GUNRETURN

namespace kraken::fix::gunreturn {
    // When the player's guns haven't fired (and the player isn't holding fire)
    // for [gunreturn] timeout seconds, ease them back to their forward rest
    // position. Ported from VariousHacks/GunRotation.h. No-op unless enabled.
    void Apply();
}

#endif
