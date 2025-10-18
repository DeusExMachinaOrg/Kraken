#pragma once
#include "utils.hpp"
#include "hta/m3d/Class.h"
#include "hta/m3d/Object.h"
#include "hta/m3d/SgNode.hpp"

namespace m3d::cmn {
    struct XmlFile;
    struct XmlNode;
};


namespace m3d {
    class SgSoundSourceNode : public SgNode {
        /* Size=0x1f4 */
        /* 0x0000: fields for m3d::SgNode */
        /* 0x01d4 */ uint32_t m_props[6];
        /* 0x01ec */ int32_t m_currentSoundNum;
        /* 0x01f0 */ int32_t m_framesPassed;

        static m3d::Class m_classSgSoundSourceNode;

        SgSoundSourceNode();
        SgSoundSourceNode(const m3d::SgSoundSourceNode&);
        virtual ~SgSoundSourceNode();
        virtual m3d::Object* Clone();
        virtual m3d::Class* GetClass() const;
        virtual int32_t ReadFromXmlNode(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*);
        virtual int32_t WriteToXmlNode(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*);
        virtual int32_t SetProperty(uint32_t, void*);
        virtual int32_t GetProperty(uint32_t, void*) const;
        virtual int32_t GetPropertiesList(stable_size_set<unsigned int>&) const;
        virtual m3d::DataServer* GetServer() const;
        virtual int32_t Render(m3d::SgNodeRenderFlags, void*, int32_t, int32_t);
        virtual float IntersectRay(const CVector&, const CVector&, m3d::SgNode*&, m3d::Class*);
        virtual bool IsFree() const;
        virtual void CanBeFree();
        virtual void Restart();
        virtual void UpdateOwnBoundingBox();
        int32_t _InternalRender();
        bool _OnSoundStopped();

        public: static m3d::Object* CreateObject();
        public: static m3d::Class* GetBaseClass();
    };
};