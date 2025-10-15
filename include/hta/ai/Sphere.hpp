#ifndef HTA_AI_SPHERE
#define HTA_AI_SPHERE

#include "ode/ode.hpp"
#include "hta/ai/Geom.h"

namespace ai {
    struct Sphere : public Geom {
        /* Size=0x18 */
        /* 0x0000: fields for Geom */

        float GetRadius() const {
            dGeomSphereGetRadius(this->m_geomId);
        };

        void SetRadius(float);
        Sphere(dxGeom*, void (*)(dxGeom*));
        virtual ~Sphere();

        static Sphere* CreateObject(dxSpace*, float, void (*)(dxGeom*));
    };
};

#endif