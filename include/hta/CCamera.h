#pragma once
#include "CVector.h"

struct CAffineXForm // sizeof=0x1C
{
    void* __vftable;
    CVector m_worldOrigin;
    float m_rotYaw;
    float m_rotPitch;
    float m_rotRoll;
};

struct CCamera : CAffineXForm // sizeof=0x24
{
    float m_fovX;
    float m_fovY;
};