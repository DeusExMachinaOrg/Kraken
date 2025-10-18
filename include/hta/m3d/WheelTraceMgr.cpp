#include "hta/m3d/WheelTraceMgr.hpp"

namespace m3d {
    int32_t WheelTraceMgr::StartSkidding(void* object, int32_t soil_id) {
        FUNC(0x00698BB0, int32_t, __thiscall, jump, WheelTraceMgr*, void*, int32_t);
        return jump(this, object, soil_id);
    };

    bool WheelTraceMgr::IsSkiddingStarted(void* object) {
        FUNC(0x00698700, bool, __thiscall, jump, WheelTraceMgr*, void*);
        return jump(this, object);
    };

    void WheelTraceMgr::AddTrace(const CVector& pos, const Quaternion& rot, float size, void* object, int32_t soil_id, bool smooth) {
        FUNC(0x00698CE0, void, __thiscall, jump, WheelTraceMgr*, const CVector*, const Quaternion*, float, void*, int32_t, bool);
        jump(this, &pos, &rot, size, object, soil_id, smooth);
    };
    
    int32_t WheelTraceMgr::EndSkidding(void* wheel, bool smooth) {
        FUNC(0x00698630, int32_t, __thiscall, jump, WheelTraceMgr*, void*, bool);
        return jump(this, wheel, smooth);
    };
};