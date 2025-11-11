#include "hta/m3d/AIParam.hpp"

namespace m3d {
    void AIParam::Detach() {
        FUNC(0x00404FF0, void, __thiscall, _Detach, const AIParam*);
        _Detach(this);
    };
}