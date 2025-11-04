#pragma once
#include "hta/CStr.h"
#include "hta/m3d/Class.h"
#include "hta/ai/Obj.hpp"

namespace m3d::cmn
{
    struct XmlFile;
    struct XmlNode;
    struct Class;
}

namespace ai
{
    struct PrototypeInfo { /* Size=0x40 */
        /* 0x0004 */ CStr m_className;
        /* 0x0010 */ CStr m_prototypeName;
        /* 0x001c */ int32_t m_prototypeId;
        /* 0x0020 */ int32_t m_resourceId;
        /* 0x0024 */ bool m_bIsUpdating;
        /* 0x0025 */ bool m_bVisibleInEncyclopedia;
        /* 0x0026 */ bool m_bApplyAffixes;
        /* 0x0028 */ uint32_t m_price;
        /* 0x002c */ bool m_bIsAbstract;
        /* 0x0030 */ CStr m_parentPrototypeName;
        /* 0x003c */ m3d::Class* m_protoClassObject;
        
        PrototypeInfo();
        PrototypeInfo(const PrototypeInfo&);
        virtual ~PrototypeInfo();
        virtual bool LoadFromXML(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual void PostLoad();
        virtual void RefreshFromXml(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual Obj* CreateTargetObject() const;
        bool bIsAbstract() const;
        const CStr& GetParentPrototypeName() const;
        virtual uint32_t GetBasePrice() const;
        void CopyFrom(const PrototypeInfo&);
        CStr GetDebugDescription() const;
        bool IsPrototypeOf(const m3d::Class*) const;
        virtual void _InternalCopyFrom(const PrototypeInfo&);
    };
}

ASSERT_SIZE(ai::PrototypeInfo, 0x40);