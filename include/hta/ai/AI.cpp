#include "hta/ai/AI.hpp"
#include "AI.hpp"
#include "hta/Func.h"

namespace ai {
    const CStr& AI::GetCurState2Name() {
        FUNC(0x007F1FF0, const CStr&, __thiscall, _GetCurState2Name, const AI*);
        return _GetCurState2Name(this);
    }

    void AI::InsCommand(int32_t Num, const m3d::AIParam& Param1, const m3d::AIParam& Param2, const m3d::AIParam& Param3) {
        FUNC(0x007F4A80, void, __thiscall, _InsCommand, const AI*, int32_t, const m3d::AIParam*, const m3d::AIParam*, const m3d::AIParam*);
        return _InsCommand(this, Num, &Param1, &Param2, &Param3);
    }
}