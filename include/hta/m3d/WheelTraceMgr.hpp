#pragma once
#include "utils.hpp"
#include "hta/CVector.h"
#include "hta/Quaternion.h"
#include "hta/m3d/rend/IEffect.hpp"
#include "hta/m3d/rend/TexHandle.hpp"
#include "hta/m3d/rend/VbHandle.hpp"
#include "hta/m3d/rend/IbHandle.hpp"

namespace m3d {
    struct SkidStrip;
    struct Profiler;

    struct WheelTraceMgr {
        /* Size=0x30 */
        /* 0x0000 */ SkidStrip* m_skidStrips;
        /* 0x0004 */ stable_size_map<void*,int> m_ownersToIdxMap;
        /* 0x0010 */ stable_size_vector<rend::TexHandle> m_texHandles;
        /* 0x0020 */ rend::VbHandle m_vb;
        /* 0x0024 */ rend::IbHandle m_ib;
        /* 0x0028 */ rend::IEffect* m_shader;
        /* 0x002c */ Profiler* m_profiler;
    
        WheelTraceMgr(const WheelTraceMgr&);
        WheelTraceMgr();
        ~WheelTraceMgr();
        void Init(int32_t);
        void Release();
        void ClearTraces();
        void AddTextureBySoilType(int32_t, const CStr&);
        void Render();
        int32_t StartSkidding(void*, int32_t);
        int32_t EndSkidding(void*, bool);
        bool IsSkiddingStarted(void*);
        void AddTrace(const CVector&, const Quaternion&, float, void*, int32_t, bool);
    };
};