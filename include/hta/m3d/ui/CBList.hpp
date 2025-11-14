#pragma once

#include "hta/m3d/ui/Wnd.hpp"
#include "hta/m3d/ui/CBButton.hpp"

namespace hta::m3d::ui {
    struct ButtonWnd;
}

namespace hta::ai {
    struct Workshop;
    struct GeomRepository;
};

namespace hta::m3d::ui {
    struct CBList : public m3d::ui::Wnd { /* Size=0x28c */
        struct CBList::AuxInfo { /* Size=0x2c */
            /* 0x0000 */ public: CStr m_btnNextName;
            /* 0x000c */ public: CStr m_btnPrevName;
            /* 0x0018 */ public: CStr m_wndListBgName;
            /* 0x0024 */ public: float m_space;
            /* 0x0028 */ public: float m_edge;

            AuxInfo(const CBList::AuxInfo&);
            AuxInfo();
            ~AuxInfo();
        };
        static_assert(sizeof(CBList::AuxInfo) == 0x2c);

        /* 0x0000: fields for m3d::ui::Wnd */
        /* 0x0220 */ protected: m3d::ui::ButtonWnd* m_btnNext;
        /* 0x0224 */ protected: m3d::ui::ButtonWnd* m_btnPrev;
        /* 0x0228 */ protected: m3d::ui::Wnd* m_wndListBg;
        /* 0x022c */ protected: CBList::AuxInfo m_aif;
        /* 0x0258 */ protected: vc3::vector<CBButton * > m_items;
        /* 0x0268 */ protected: BoundsBase<float> m_listBounds;
        /* 0x0278 */ protected: int32_t m_firstItemId;
        /* 0x027c */ protected: int32_t m_lastItemId;
        /* 0x0280 */ protected: int32_t m_workshopId;
        /* 0x0284 */ protected: CBButton::Type m_type;
        /* 0x0288 */ protected: int32_t m_selItemId;
        static m3d::Class m_classCBList;
        
        int32_t SetupForWorkshop(int32_t);
        virtual int32_t GameDataSetup();
        virtual int32_t GameDataUpdate(void*, int32_t);
        virtual int32_t GameDataClear(bool);
        virtual int32_t OnWndNotify(m3d::ui::Wnd*, uint32_t, uint32_t, const m3d::AIParam&);
        virtual int32_t OnBeforeAddToWndStation();
        virtual int32_t OnAfterAddToWndStation();
        virtual int32_t OnAfterRemoveFromWndStation();
        virtual int32_t OnMouseWheel(int32_t, const PointBase<float>&);
        int32_t AddItem(int32_t);
        int32_t RemoveItem(int32_t);
        void RecalcLayot();
        void ScrollNext();
        void ScrollPrev();
        void UpdateNextPrevButtonState();
        bool CanScrollNext();
        bool CanScrollPrev();
        void ClearItems();
        void OnBuyCB(int32_t);
        ai::Workshop* GetWorkshop() const;
        ai::GeomRepository* GetWorkshopRepository() const;
        void FullUpdate();
        int32_t CreateItems();
        void GetCBIds(vc3::vector<int>&) const;
        virtual int32_t GetCBResourceId() const;
        virtual CBButton* CreateItem() const;
        virtual void OnRepositoryChanged();
        virtual void SelectItem(CBButton*);
        void HackedRestore();
        void AddInfoToEncyclopaedia();
        virtual void PostTriggerEventOnBuyCb();
        CBList(const CBList&);
        CBList();
        virtual ~CBList();
        virtual m3d::Class* GetRtClass() const;

        static m3d::Class* GetBaseClass();
    };
    static_assert(sizeof(CBList) == 0x28c);
}