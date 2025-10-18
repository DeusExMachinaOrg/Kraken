#pragma once

#include "hta/CVector.h"
#include "hta/CMatrix.hpp"

namespace m3d::cmn {
    struct XmlFile;
    struct XmlNode;
};

class CAffineXForm {
    /* Size=0x1c */
    /* 0x0004 */ CVector m_worldOrigin;
    /* 0x0010 */ float m_rotYaw;
    /* 0x0014 */ float m_rotPitch;
    /* 0x0018 */ float m_rotRoll;
  
    CAffineXForm(const CAffineXForm&);
    CAffineXForm();
    void createViewMatrix(CMatrix&) const;
    void createRotationMatrix(CMatrix&) const;
    void MoveAlong(const CVector&);
    virtual void LoadFromXml(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
    virtual void SaveToXml(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
};