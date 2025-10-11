#pragma once
#include "utils.hpp"
#include "hta/m3d/AIParam.h"

namespace ai {
    struct AIPassageState // sizeof=0x14
    {
        int m_StateNum;
        stable_size_vector<m3d::AIParam> m_ParamList;
    };
}