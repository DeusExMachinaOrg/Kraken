#pragma once

#include "ode/ode.hpp"
#include "hta/CVector.h"
#include "hta/ai/Geom.h"

namespace ai {
    struct Box : public Geom {
        /* Size=0x18 */
        /* 0x0000: fields for ai::Geom */

        CVector GetSize() const;
        void SetSize(const CVector&);
        Box(dxGeom*, void (*)(dxGeom*));
        virtual ~Box();

        static Box* CreateObject(dxSpace*, const CVector&, void (*)(dxGeom*));
    };
};