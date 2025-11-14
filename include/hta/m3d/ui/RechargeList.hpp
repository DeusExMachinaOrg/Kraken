#pragma once

#include "hta/m3d/ui/AdvancedList.hpp"

namespace hta::m3d::ui {
    struct RechargeList : public AdvancedList { /* Size=0x248 */
        /* 0x0000: fields for AdvancedList */
        static m3d::Class m_classRechargeList;

        virtual void BuyService(const AdvancedButton*);
        virtual AdvancedButton* NewItem() const;
        virtual vc3::vector<int> GetObjIds() const;
        RechargeList();
        RechargeList(const RechargeList&);
        virtual ~RechargeList();
        virtual m3d::Object* Clone();
        virtual m3d::Class* GetClass() const;

        static m3d::Object* CreateObject();
        static m3d::Class* GetBaseClass();
    };
    static_assert(sizeof(RechargeList) == 0x248);
}