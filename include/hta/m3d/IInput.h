#pragma once
#include "hta/IBase.h"
#include "hta/m3d/Kernel.h"


namespace m3d {
    namespace input {
        enum Language : __int32
        {
            LANGUAGE_BASE       = 0x0,
            LANGUAGE_ADDITIONAL = 0x1,
        };

        enum DeviceParam : __int32
        {
            DP_MOUSE_X    = 0x0,
            DP_MOUSE_Y    = 0x1,
            DP_MOUSE_Z    = 0x2,
            DP_MOUSE_B    = 0x3,
            DP_JOY_X      = 0x4,
            DP_JOY_Y      = 0x5,
            DP_JOY_Z      = 0x6,
            DP_JOY_RX     = 0x7,
            DP_JOY_RY     = 0x8,
            DP_JOY_RZ     = 0x9,
            DP_JOY_B      = 0xA,
            DP_JOY_S0     = 0xB,
            DP_JOY_S1     = 0xC,
            DP_JOY_P0     = 0xD,
            DP_JOY_P1     = 0xE,
            DP_JOY_P2     = 0xF,
            DP_JOY_P3     = 0x10,
            DP_NUM_PARAMS = 0x11,
        };

        struct IInput : IBase {
            virtual int Init(m3d::Kernel*, void (__fastcall*)(const CStr&)) = 0 /* 0x10 */;
            virtual void NewFrame() = 0 /* 0x14 */;
            virtual void SetAutorepeatTime(int) = 0 /* 0x18 */;
            virtual void ClearBuffer() = 0 /* 0x1c */;
            virtual bool GetLastKbdEvent(unsigned short&, unsigned char&, bool&, double&, bool) = 0 /* 0x20 */;
            virtual int GetMouseX() = 0 /* 0x24 */;
            virtual int GetMouseY() = 0 /* 0x28 */;
            virtual int GetMouseZ() = 0 /* 0x2c */;
            virtual int GetMouseB(int) = 0 /* 0x30 */;
            virtual int GetParam(m3d::input::DeviceParam) = 0 /* 0x34 */;
            virtual int SetActiveState(int) = 0 /* 0x38 */;
            virtual void ChangeLanguage() = 0 /* 0x3c */;
            virtual void SetLanguage(m3d::input::Language) = 0 /* 0x40 */;
            virtual m3d::input::Language GetLanguage() const = 0 /* 0x44 */;
        };
    }
}