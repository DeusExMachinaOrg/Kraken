#pragma once
#include <utils.hpp>
#include "hta/ai/ObjContainer.hpp"

namespace ai
{
    float ObjContainer::GetGameTimeDiff() const
    {
        FUNC(0x0062D230, float, __thiscall, _GetGameTimeDiff, const ObjContainer*);
        return _GetGameTimeDiff(this);
    }

    Obj* ObjContainer::GetEntityByObjId(std::int32_t objId)
    {
        FUNC(0x0040C310, Obj*, __thiscall, _GetEntityByObjId,ObjContainer*, std::int32_t);
        return _GetEntityByObjId(this, objId);
    }
}
