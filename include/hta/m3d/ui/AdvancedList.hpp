#pragma once

#include "hta/m3d/ui/Wnd.hpp"

class AdvancedButton;

namespace hta::ai {
    struct Workshop;
};

namespace hta::m3d::ui {
    struct AdvancedList : public m3d::ui::Wnd { /* Size=0x248 */
        struct AdvancedList::AuxInfo { /* Size=0x4 */
            /* 0x0000 */ public: float m_space;
            public: AuxInfo();
        };
        static_assert(sizeof(AdvancedList::AuxInfo) == 0x4);

        /* 0x0000: fields for m3d::ui::Wnd */
        /* 0x0220 */ protected: vc3::vector<AdvancedButton *> m_items;
        /* 0x0230 */ protected: BoundsBase<float> m_switchB;
        /* 0x0240 */ protected: AdvancedList::AuxInfo m_aif;
        /* 0x0244 */ protected: int32_t m_workshopId;
        static m3d::Class m_classAdvancedList;
        
        int32_t SetupForWorkshop(int32_t);
        void SetSwitchWndBounds(const BoundsBase<float>&);
        virtual int32_t GameDataSetup();
        virtual int32_t GameDataClear(bool);
        virtual int32_t GameDataUpdate(void*, int32_t);
        virtual int32_t OnWndNotify(m3d::ui::Wnd*, uint32_t, uint32_t, const m3d::AIParam&);
        virtual int32_t OnBeforeAddToWndStation();
        void ClearItems();
        virtual void RecalcLayot();
        virtual void BuyService(const AdvancedButton*);
        virtual int32_t CreateItems();
        virtual int32_t AddItem(int32_t);
        virtual AdvancedButton* NewItem() const;
        virtual void OnPlayerVehicleChanged();
        virtual void OnVehiclePartChanged(void*);
        virtual void OnNewFrame();
        virtual vc3::vector<int> GetObjIds() const;
        ai::Workshop* GetWorkshop() const;
        void OnAdvancedModeChanged(m3d::ui::Wnd*, const m3d::AIParam&);
        void SetAdvancedModeForButton(AdvancedButton*);
        AdvancedList(const AdvancedList&);
        AdvancedList();
        virtual ~AdvancedList();
        virtual m3d::Class* GetRtClass() const;

        static m3d::Class* GetBaseClass();
    };
    static_assert(sizeof(AdvancedList) == 0x248);
}