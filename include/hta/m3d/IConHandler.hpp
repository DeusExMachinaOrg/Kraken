#pragma once

#include <stdint.h>

namespace m3d {
    struct CVar;
    struct CConsoleParams;

    struct IConHandler {
        /* Size=0x4 */
        virtual void HandleCommand(int32_t, const CConsoleParams&);
        virtual bool HandleCVar(const CVar*, const CConsoleParams&);
        IConHandler(const IConHandler&);
        IConHandler();
    };
};