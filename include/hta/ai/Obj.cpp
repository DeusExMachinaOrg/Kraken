#include "hta/ai/Obj.hpp"
#include "hta/ai/IPriceCoeffProvider.h"

namespace ai {
    int32_t Obj::OnEvent(const Event& evn)
    {
        FUNC(0x00692FB0, int32_t, __thiscall, _OnEvent, Obj*, const Event&);
        return _OnEvent(this, evn);
    }

    float Obj::GetPriceCoeff(const IPriceCoeffProvider* provider) const
    {
        FUNC(0x006895C0, double, __thiscall, _GetPriceCoeff, const Obj*, const ai::IPriceCoeffProvider*);
        return _GetPriceCoeff(this, provider);
    }
}