#include "utils.hpp"
#include "hta/m3d/AIParam.h"

namespace ai {
    struct AIMessage // sizeof=0x18
    {
        int m_Num;
        int m_RemoveAfterFinishing;
        stable_size_vector<m3d::AIParam> m_ParamList;
    };
}