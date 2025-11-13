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

    int ObjContainer::CreateNewObject(int prototypeId, const char *name, int parentId, int belongId)
    {
        FUNC(0x00633C60, int, __thiscall, _CreateNewObject, ObjContainer*, int, const char*, int, int);
        return _CreateNewObject(this, prototypeId, name, parentId, belongId);
    }

    int32_t ObjContainer::GetPrototypeId(const char* prototypeName) const
    {
        FUNC(0x0062D7D0, int32_t, __thiscall, _GetPrototypeId, const ObjContainer*, const char*);
        return _GetPrototypeId(this, prototypeName);
    }
}
