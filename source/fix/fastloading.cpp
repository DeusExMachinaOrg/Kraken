#define LOGGER "fastloading"

#include <climits>

#include "ext/logger.hpp"
#include "fix/fastloading.hpp"
#include "routines.hpp"

#include "hta/m3d/Application.hpp"
#include "hta/m3d/rend/IRenderer.hpp"
#include "hta/m3d/ui/ProgressBarWnd.hpp"

namespace kraken::fix::fastloading
{
    // Pixel position the bar was at the last time we actually presented a frame.
    static int presentedPixel = INT_MIN;
    // Pixel position of the bar as of the most recent SetCurValue call.
    static int currentPixel = INT_MIN;

    // Hooks SplashWnd::ShowSplash's call to ProgressBarWnd::SetCurValue, so we
    // learn the bar's new value without needing SplashWnd's own instance pointer.
    void __fastcall TrackLoadingProgress(hta::m3d::ui::ProgressBarWnd* self, int, float curValue)
    {
        self->SetCurValue(curValue);
        currentPixel = (int)self->GetValueInPixel();
    }

    // Only present a frame when the bar has actually moved by a pixel since the
    // last present, instead of on a fixed frame-count cadence.
    void SkipLoadingPresentScene()
    {
        if (currentPixel == presentedPixel)
            return;

        presentedPixel = currentPixel;
        hta::m3d::Application::Instance()->m_renderer->PresentScene();
    }

    void Apply()
    {
        LOG_INFO("Feature enabled");
        routines::ChangeCall((void*)0x004C8A9A, TrackLoadingProgress);
        routines::ReplaceCall((void*)0x004C8BBE, SkipLoadingPresentScene);
    }
}