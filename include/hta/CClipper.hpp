#pragma once

#include <stdint.h>
#include <hta/CVector.h>
#include <hta/CMatrix.hpp>

enum tbEnum : int32_t {
    tbFullTest = 0x0000,
    tbRejectOnly = 0x0001,
};

class CClipper {
    /* Size=0x418 */
    /* 0x0000 */ float m_planes[26][4];
    /* 0x01a0 */ uint32_t m_indices[156];
    /* 0x0410 */ uint32_t m_enabled;
    /* 0x0414 */ uint32_t m_nfrustums;

    void createIndices();
    void createScreenFrustums(const CVector&, const CMatrix&, float, float, float, float);
    void CreateScreenFrustums(float, float, float, float);
    void enableAll();
    int32_t enableSetFromBox(float*, const CVector&);
    void enableUpdateFromBox(float*, const CVector&);
    uint32_t enableGetState() const;
    void enableSetState(uint32_t);
    int32_t testBBox(tbEnum, float*, const CVector&) const;
    int32_t testSphere(const CVector&, float) const;
    int32_t testVertexInside(const CVector&);
    int32_t clipPolyInPlace(CVector*, int32_t);
    int32_t clipPolyInPlaceZ(CVector*, int32_t);
    int32_t clipLineZ(CVector*, CVector*);
    void CreateFromWinding(int32_t, CVector*, int32_t, const CVector&, CPlane*);
    void translate(const CVector&);

    static void buildfrustum(float*, const CVector*, const CMatrix&, const CVector&, float);
    static void buildfrustum(float*, const CVector&, const CMatrix&, const CVector&, float);
};
