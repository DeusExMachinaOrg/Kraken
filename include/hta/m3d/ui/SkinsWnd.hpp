#pragma once

#include "hta/m3d/ui/Wnd.hpp"

namespace hta::m3d::ui {
    struct ButtonWnd;
};

namespace hta::ai {
    struct Vehicle;
};

class SkinSwitcher;

namespace hta::m3d::ui {
    struct SkinsWnd : public m3d::ui::Wnd { /* Size=0x26c */
        struct SkinsWnd::AuxInfo { /* Size=0x30 */
            /* 0x0000 */ CStr m_btnPrevName;
            /* 0x000c */ CStr m_btnNextName;
            /* 0x0018 */ CStr m_btnBuyName;
            /* 0x0024 */ CStr m_wndPriceName;
            
            AuxInfo(const SkinsWnd::AuxInfo&);
            AuxInfo();
            ~AuxInfo();
        };
        /* 0x0000: fields for m3d::ui::Wnd */
        /* 0x0220 */ SkinSwitcher* m_skinSwitcher;
        /* 0x0224 */ m3d::ui::ButtonWnd* m_btnPrev;
        /* 0x0228 */ m3d::ui::ButtonWnd* m_btnNext;
        /* 0x022c */ m3d::ui::ButtonWnd* m_btnBuy;
        /* 0x0230 */ m3d::ui::Wnd* m_wndPrice;
        /* 0x0234 */ SkinsWnd::AuxInfo m_aif;
        /* 0x0264 */ int32_t m_vehicleId;
        /* 0x0268 */ int32_t m_savedSkinId;
        static m3d::Class m_classSkinsWnd;

        int32_t SetupForVehicle(int32_t);
        virtual int32_t GameDataSetup();
        virtual int32_t OnWndNotify(m3d::ui::Wnd*, uint32_t, uint32_t, const m3d::AIParam&);
        virtual int32_t OnBeforeAddToWndStation();
        virtual int32_t OnAfterRemoveFromWndStation();
        void BuySkin();
        int32_t GetSkinPrice(int32_t, int32_t) const;
        ai::Vehicle* GetVehicle() const;
        void FullUpdate();
        void ShowNextSkin();
        void ShowPrevSkin();
        void OnChangeSkin();
        SkinsWnd();
        SkinsWnd(const SkinsWnd&);
        virtual ~SkinsWnd();
        virtual m3d::Object* Clone();
        virtual m3d::Class* GetClass() const;

        static m3d::Object* CreateObject();
        static m3d::Class* GetBaseClass();
    };
}