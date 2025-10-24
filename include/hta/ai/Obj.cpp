#include "hta/ai/Obj.h"

namespace ai {

    GeomRepository* Obj::GetParentRepository() const
    {
        FUNC(0x006894A0, GeomRepository*, __thiscall, _GetParentRepository, const Obj*);
        return _GetParentRepository(this);
    };

};