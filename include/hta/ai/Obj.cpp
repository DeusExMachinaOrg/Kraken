#include "hta/ai/Obj.hpp"

namespace ai {
    int32_t Obj::OnEvent(const Event& evn)
    {
        FUNC(0x00692FB0, int32_t, __thiscall, _OnEvent, Obj*, const Event&);
        return _OnEvent(this, evn);
    }
}