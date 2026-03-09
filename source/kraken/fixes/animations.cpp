#define LOGGER "animations"

#include "config/config.hpp"
#include "logger/logger.hpp"
#include "common/routines.hpp"
#include "fixes/animations.hpp"
#include "hta/m3d/AnimInfo.hpp"

namespace kraken::fix::animations {
    void __fastcall MoveFrame(hta::m3d::AnimInfo *self, void* _, unsigned int dti) {
        auto* anim = self->m_curAnimation;

        if (!anim || anim->m_fps < 1) {
            self->m_curAnimFrame = 0;
            return;
        }

        self->m_lastInterpolationUpdate = -1;
        self->m_timeOutToNextFrame -= dti;

        if (self->m_timeOutToNextFrame > 0)
            return;

        int elapsed   = -self->m_timeOutToNextFrame;
        int advance   = 1 + elapsed / anim->m_fps;
        int frame     = self->m_curAnimFrame + advance;
        int numFrames = anim->m_numFrames;

        self->m_curAnimFrame = frame;
        self->m_timeOutToNextFrame += advance * anim->m_fps;

        if (frame >= numFrames - 1) {
            int next = anim->m_nextAnimation;

            if (next >= 0) {
                bool isSameAnim = (anim - self->m_forModel->m_animations) == next;
                if (isSameAnim)
                    self->m_curAnimFrame = frame % numFrames;
                else
                    self->SetAnimationIdx(next);
            } else {
                self->m_stickToLastFrame = 1;
                self->m_curAnimFrame = numFrames - 1;
            }
        }

        self->m_blendFramesNum -= advance;
        if (self->m_isBlending && self->m_blendFramesNum < 0)
            self->m_isBlending = false;
    };

    void Apply() {
        LOG_INFO("Feature enabled");
        routines::Redirect(0xE0, (void*) 0x0070A620, &MoveFrame);
    };
};