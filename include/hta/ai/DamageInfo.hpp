#pragma once
#include "hta/CVector.h"
#include "hta/CStr.h"

namespace ai
{
    enum DamageType
    {
        DAMAGE_PIERCING = 0x0,
        DAMAGE_BLAST = 0x1,
        DAMAGE_ENERGY = 0x2,
        DAMAGE_WATER = 0x3,
        DAMAGE_NUM_TYPES = 0x4,
    };

    struct DamageInfo
    {
        DamageInfo();

        int attackerId;
        int attackingAgentId;
        bool bDamageFriends;
        int gunPrototypeId;
        float damage;
        DamageType damageType;
        CStr damagedPartName;
        CVector hitPos;
        CVector hitDir;
        CVector normal;
        int decalId;
    };
}
