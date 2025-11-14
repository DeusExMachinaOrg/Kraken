#pragma once

#include "hta/m3d/ui/Wnd.hpp"
#include "hta/ref_ptr.hpp"

namespace hta::m3d::ui {
    struct RepositoryItemWnd;
    struct ButtonWnd;
}

namespace hta::m3d::ui {
    struct CBButton : public m3d::ui::Wnd { /* Size=0x250 */
        enum Type : int32_t {
            TYPE_CABIN = 0x0000,
            TYPE_BASKET = 0x0001,
            NUM_TYPES = 0x0002,
        };

        struct CBButton::AuxInfo { /* Size=0x78 */
            /* 0x0000 */ public: CStr m_wndNameName;
            /* 0x000c */ public: CStr m_btnBuyName;
            /* 0x0018 */ public: CStr m_wndPictureName;
            /* 0x0024 */ public: CStr m_wndDurabilityName;
            /* 0x0030 */ public: CStr m_wndResistPiercingName;
            /* 0x003c */ public: CStr m_wndResistBlastName;
            /* 0x0048 */ public: CStr m_wndResistEnergyName;
            /* 0x0054 */ public: CStr m_wndWeightName;
            /* 0x0060 */ public: CStr m_wndPriceName;
            /* 0x006c */ public: CStr m_selPaneName;
            
            AuxInfo(const CBButton::AuxInfo&);
            AuxInfo();
            ~AuxInfo();
        };
        /* 0x0000: fields for m3d::ui::Wnd */
        /* 0x0220 */ int32_t m_cbId;
        /* 0x0224 */ m3d::ui::Wnd* m_wndName;
        /* 0x0228 */ m3d::ui::ButtonWnd* m_btnBuy;
        /* 0x022c */ ref_ptr<RepositoryItemWnd> m_wndPicture;
        /* 0x0230 */ m3d::ui::Wnd* m_wndDurability;
        /* 0x0234 */ m3d::ui::Wnd* m_wndResistPiercing;
        /* 0x0238 */ m3d::ui::Wnd* m_wndResistBlast;
        /* 0x023c */ m3d::ui::Wnd* m_wndResistEnergy;
        /* 0x0240 */ m3d::ui::Wnd* m_wndWeight;
        /* 0x0244 */ m3d::ui::Wnd* m_wndPrice;
        /* 0x0248 */ CBButton::Type m_type;
        /* 0x024c */ bool m_bSelected;
        static CBButton::AuxInfo m_aif;
        static ref_ptr<m3d::ui::Wnd> m_wndPattern[2];
        static m3d::Class m_classCBButton;

        int32_t SetupForCB(int32_t);
        int32_t GetCBId() const;
        int32_t GetPrice() const;
        void Select(bool);
        bool IsSelected() const;
        virtual int32_t CreateFromPattern();
        virtual int32_t CreateChildren();
        virtual int32_t OnWndNotify(m3d::ui::Wnd*, uint32_t, uint32_t, const m3d::AIParam&);
        virtual int32_t FullUpdate();
        CBButton();
        CBButton(const CBButton&);
        virtual ~CBButton();
        virtual m3d::Object* Clone();
        virtual m3d::Class* GetClass() const;

        static int32_t LoadPattern(ref_ptr<m3d::ui::Wnd>, CBButton::Type);
        static void ClearPattern(CBButton::Type);
        static m3d::Object* CreateObject();
        static m3d::Class* GetBaseClass();
    };
}
