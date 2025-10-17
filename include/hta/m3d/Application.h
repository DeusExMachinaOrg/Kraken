#pragma once
#include <stdafx.hpp>
#include "GameImpulse.h"
#include "IRenderer.h"
#include "hta/CCamera.h"
#include "hta/m3d/IInput.h"
#include "hta/ai/AIParam.h"

namespace snd {
    class ISound;
}

namespace m3d
{
    struct Cinematic;
    struct Log;
    struct IEventHandler;

    class Event {
    public:
        long double m_timeStamp = 0.0;
        int m_eventType = 0;
        CStr m_strEv;
        AIParam m_aiParamEv;
        union {
            void* m_void[4] = { 0 };
            IEventHandler* m_handle[4];
            unsigned int m_uintEv[4];
            int m_intEv[4];
            unsigned __int16 m_ushortEv[8];
            __int16 m_shortEv[8];
            unsigned __int8 m_byteEv[16];
        };
        Event() {}
        ~Event() {}
    };
}

namespace m3d
{
    struct Application // sizeof=0x8B290
    {
        struct MouseInfo // sizeof=0x10
        {                                       // XREF: m3d::Application/r
            PointBase<int> m_deltaDuringGameFrame;
            PointBase<int> m_lastPos;
        };

        struct DetailSettings // sizeof=0x10
        {                                       // XREF: m3d::Application/r
            bool m_dsShadows;
            // padding byte
            // padding byte
            // padding byte
            BYTE _offset1[0x3];
            float m_lsViewDistanceDivider;
            int m_lsWaterMethod;
            float m_NPatchLevel;
        };

        inline static Application*& Instance = *(Application**)0x00A0A55C;

        BYTE _offset[0x2FC];
        m3d::rend::IRenderer* m_renderer;
        snd::ISound* m_sound;
        m3d::input::IInput* m_input;
        m3d::Log* m_log;
        CStr m_cfgName;
        unsigned int m_dwWindowStyle;
        tagRECT m_rcWindowBounds;
        tagRECT m_rcWindowClient;
        CStr m_frameStats;
        unsigned int m_frameFillRate;
        char *m_strWindowTitle;
        CStr m_startupFolder;
        CStr m_imageName;
        int m_appNeedToRedraw;
        int m_screenShotPending;
        int m_screenShotPendingAlways;
        void *m_procTexNewTextureNotifyEvent;
        void *m_procTexShutdownEvent;
        void *m_procTexThreadHandle;
        unsigned int m_procTexThreadId;
        // padding byte
        // padding byte
        // padding byte
        // padding byte
        BYTE pad1[0x4];
        m3d::Event m_eventsQueue[5000];
        int m_eventsQueueHead;
        int m_eventsQueueTail;
        m3d::IEventHandler *m_focusKbdEntity;
        int m_mouseX;
        int m_mouseY;
        float m_mouseSensitivity;
        bool m_bMouseYAxisFlipped;
        bool m_bMouseXAxisFlipped;
        // padding byte
        // padding byte
        BYTE pad2[0x2];
        std::vector<m3d::IEventHandler *> m_allEventHandlers;
        HINSTANCE__ *m_hInputDll;
        HINSTANCE__ *m_hRenderDll;
        HINSTANCE__ *m_hSoundDll;
        bool m_breakLoop;
        // padding byte
        // padding byte
        // padding byte
        BYTE pad3[0x3];
        int m_isAppActive;
        BYTE _offset0[0x33024];
        bool m_bDoNotLoadMainmenuLevel;
        // padding byte
        // padding byte
        // padding byte
        BYTE _offset1[0x3];
        m3d::Application::DetailSettings m_detailSettings;
        IImpulse* m_pImpulses;
        bool m_bDXCursorEnabled;
        // padding byte
        // padding byte
        // padding byte
        BYTE _offset2[0x3];
        m3d::Application::MouseInfo m_mouseInfo;
        HWND__ *m_renderWindow;
        m3d::IConHandler *m_soundConHandler;
        bool m_bGuiWasHiddenBeforeCinematic;
        // padding byte
        // padding byte
        // padding byte
        BYTE _offset3[0x3];
        m3d::Cinematic *m_cinematic;
        CCamera m_curCamera;
        // padding byte
        // padding byte
        // padding byte
        // padding byte
        BYTE _offset4[0x4];

        static void __fastcall PutSplashCallBack(int proc, const char** data)
        {
            if (!Instance || !data || !*data)
                return;
            Instance->PutSplash(proc, *data);
        }

        void PutSplash(int proc, const char* data)
        {
            FUNC(0x006A5A70, void, __thiscall, _PutSplash, Application*, int, const char**);
            _PutSplash(this, proc, &data);
        }

        int OneFrame()
        {
            FUNC(0x005A3AD0, int, __thiscall, _OneFrame, Application*);
            return _OneFrame(this);
        }

        double GetMouseSensitivity()
        {
            FUNC(0x0059C590, double, __thiscall, _GetMouseSensitivity, Application*);
            return _GetMouseSensitivity(this);
        }
    };
}