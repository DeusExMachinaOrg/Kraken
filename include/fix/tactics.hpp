#ifndef __KRAKEN_FIX_TACTICS_HPP__
#define __KRAKEN_FIX_TACTICS_HPP__

namespace ai {
    class Team;
}

namespace kraken::fix::tactics {
    void __fastcall Fixed_AttackNow(ai::Team* team, int, int id);
    void Apply();
}

#endif