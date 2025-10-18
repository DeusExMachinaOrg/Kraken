#pragma once

#include "hta/CAffineXForm.hpp"

class CCamera : public CAffineXForm {
    /* Size=0x24 */
    /* 0x0000: fields for CAffineXForm */
    /* 0x001c */ float m_fovX;
    /* 0x0020 */ float m_fovY;

    CCamera(const CCamera&);
    CCamera();
    void lookAt(const CVector&, const CVector&);
    void lookAt(const CVector&);
    void setFov(float, float, float);
    void createProjectionMatrix(CMatrix&, float) const;
    virtual void LoadFromXml(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
    virtual void SaveToXml(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
};