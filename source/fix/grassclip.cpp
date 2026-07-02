#define LOGGER "grassclip"

#include "config.hpp"
#include "ext/logger.hpp"
#include "fix/grassclip.hpp"
#include "routines.hpp"

namespace kraken::fix::grassclip {
    // Landscape::RenderGrass (0x006B0740) renders grass alpha-BLENDED with no depth write:
    //   line 1169  PushZbState(ZB_NOWRITE)   -> 0x006B07CE: 6A 02   (push 2)
    //   line 1171  SetBlend(BM_ALPHA, 0)      -> 0x006B080B: 6A 02   (push 2)
    // The cutout already comes from the m_g_grassAlphatest alpha test, so switching the two state
    // args turns grass into a depth-writing alpha-tested opaque cutout (alpha-clip): it then sorts
    // correctly per-pixel AND writes depth, so it occludes and appears in the HBAO depth buffer.
    constexpr uintptr_t kPushZbStateArg = 0x006B07CF;  // imm of `push 2` -> ZB_NOWRITE(2) -> ZB_ENABLE(1)
    constexpr uintptr_t kSetBlendArg    = 0x006B080C;  // imm of `push 2` -> BM_ALPHA(2)   -> BM_NONE(0)

    void Apply() {
        if (!Config::Instance().grass_clip.value) {
            return;
        }

        LOG_INFO("Feature enabled");
        routines::OverrideValue((void*) kPushZbStateArg, (uint8_t) 0x01);
        routines::OverrideValue((void*) kSetBlendArg, (uint8_t) 0x00);
    }
}
