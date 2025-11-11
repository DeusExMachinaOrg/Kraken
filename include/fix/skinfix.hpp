#pragma once

namespace ai {
    struct PhysicBody;
}

namespace kraken::fix::skinfix {
    void __fastcall SetSkinFixed(ai::PhysicBody* physicBody, int, int skin);
    void Apply();
}