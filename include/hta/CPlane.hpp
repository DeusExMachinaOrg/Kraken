#pragma once

#include "hta/CVector.h"

#include <stdint.h>

class CPlane {
    /* Size=0x18 */
    /* 0x0000 */ CVector m_normal;
    /* 0x000c */ float m_dist;
    /* 0x0010 */ uint32_t m_type;
    /* 0x0014 */ uint32_t m_signbits;

    CPlane(const CPlane&);
    CPlane(float, float, float, float);
    CPlane();
    float dist(const CVector&);
    CPlane Reverse();
    void fromPointNormal(const CVector&, const CVector&);
    void normalize();
    void calcStuff();
    CVector origin();
    float intersectRay(const CVector&, const CVector&);

    static void translate(const CVector&);
    static void translate(CPlane*, CPlane*, const CVector&);
    static void buildplane(CPlane*, CVector*);
};