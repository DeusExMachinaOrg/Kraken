#pragma once

#include "hta/m3d/ui/CBList.hpp"

namespace hta::m3d::ui {
    struct CabinList : public CBList { /* Size=0x28c */
        /* 0x0000: fields for CBList */
        static m3d::Class m_classCabinList;

        virtual int32_t GetCBResourceId() const;
        virtual CBButton* CreateItem() const;
        virtual void PostTriggerEventOnBuyCb();
        CabinList();
        CabinList(const CabinList&);
        virtual ~CabinList();
        virtual m3d::Object* Clone();
        virtual m3d::Class* GetClass() const;

        static m3d::Object* CreateObject();
        static m3d::Class* GetBaseClass();
    };
    static_assert(sizeof(CabinList) == 0x28c);
}