#pragma once

#include "hta/m3d/ui/ButtonWnd.hpp"
#include "hta/m3d/ui/ChildPanel.hpp"
#include "hta/m3d/ui/CabinList.hpp"
#include "hta/m3d/ui/BasketList.hpp"
#include "hta/m3d/ui/SkinsWnd.hpp"
#include "hta/m3d/ui/RefuelList.hpp"
#include "hta/m3d/ui/RepairList.hpp"
#include "hta/m3d/ui/RechargeList.hpp"
#include "hta/Enums.hpp"
#include "hta/help.hpp"

namespace hta::ai {
    struct Workshop;
};

namespace hta::m3d::ui {
    struct GarageWnd : public ChildPanel { /* Size=0x2c8 */
        struct GarageWnd::AuxInfo { /* Size=0x1f4 */
            /* 0x0000 */ public: CStr m_btnCabinsName;
            /* 0x000c */ public: CStr m_btnBasketsName;
            /* 0x0018 */ public: CStr m_btnNewVehicleName;
            /* 0x0024 */ public: CStr m_btnSkinName;
            /* 0x0030 */ public: CStr m_btnRefuelAllName;
            /* 0x003c */ public: CStr m_btnRefuelListName;
            /* 0x0048 */ public: CStr m_wndRefuelPriceName;
            /* 0x0054 */ public: CStr m_btnRepairAllName;
            /* 0x0060 */ public: CStr m_btnRepairListName;
            /* 0x006c */ public: CStr m_wndRepairPriceName;
            /* 0x0078 */ public: CStr m_btnRechargeAllName;
            /* 0x0084 */ public: CStr m_btnRechargeListName;
            /* 0x0090 */ public: CStr m_wndRechargePriceName;
            /* 0x009c */ public: CStr m_btnBasketsTexName;
            /* 0x00a8 */ public: CStr m_btnCabinsTexName;
            /* 0x00b4 */ public: CStr m_btnSkinTexName;
            /* 0x00c0 */ public: CStr m_btnRefuelListTexName;
            /* 0x00cc */ public: CStr m_btnRepairListTexName;
            /* 0x00d8 */ public: CStr m_btnRechargeListTexName;
            /* 0x00e4 */ public: uint32_t m_colorYellow;
            /* 0x00e8 */ public: uint32_t m_colorGreen;
            /* 0x00ec */ public: uint32_t m_colorRed;
            /* 0x00f0 */ public: uint32_t m_colorTooltipRed;
            /* 0x00f4 */ public: uint32_t m_colorNormal;
            /* 0x00f8 */ public: CStr m_strIdTooltipRefuel;
            /* 0x0104 */ public: CStr m_strIdTooltipRefuelNotNeed;
            /* 0x0110 */ public: CStr m_strIdTooltipRefuelUnavailable;
            /* 0x011c */ public: CStr m_strIdTooltipRefuelPartial;
            /* 0x0128 */ public: CStr m_strIdTooltipRefuelFull;
            /* 0x0134 */ public: CStr m_strIdTooltipRepair;
            /* 0x0140 */ public: CStr m_strIdTooltipRepairNotNeed;
            /* 0x014c */ public: CStr m_strIdTooltipRepairUnavailable;
            /* 0x0158 */ public: CStr m_strIdTooltipRepairPartial;
            /* 0x0164 */ public: CStr m_strIdTooltipRepairFull;
            /* 0x0170 */ public: CStr m_strIdTooltipRecharge;
            /* 0x017c */ public: CStr m_strIdTooltipRechargeNotNeed;
            /* 0x0188 */ public: CStr m_strIdTooltipRechargeUnavailable;
            /* 0x0194 */ public: CStr m_strIdTooltipRechargePartial;
            /* 0x01a0 */ public: CStr m_strIdTooltipRechargeFull;
            /* 0x01ac */ public: CStr m_strIdState;
            /* 0x01b8 */ public: CStr m_strEnabled;
            /* 0x01c4 */ public: CStr m_strOverrolled;
            /* 0x01d0 */ public: CStr m_strPressed;
            /* 0x01dc */ public: CStr m_strDisabled;
            /* 0x01e8 */ public: CStr m_strSelected;

            AuxInfo(const GarageWnd::AuxInfo&);
            AuxInfo();
            ~AuxInfo();
        };
        static_assert(sizeof(GarageWnd::AuxInfo) == 0x1f4);

        /* 0x0000: fields for ChildPanel */
        /* 0x0224 */ m3d::ui::ButtonWnd* m_btnCabins;
        /* 0x0228 */ m3d::ui::ButtonWnd* m_btnBaskets;
        /* 0x022c */ m3d::ui::ButtonWnd* m_btnNewVehicle;
        /* 0x0230 */ m3d::ui::ButtonWnd* m_btnSkin;
        /* 0x0234 */ m3d::ui::ButtonWnd* m_btnRefuelAll;
        /* 0x0238 */ m3d::ui::ButtonWnd* m_btnRefuelList;
        /* 0x023c */ m3d::ui::Wnd* m_wndRefuelPrice;
        /* 0x0240 */ m3d::ui::ButtonWnd* m_btnRepairAll;
        /* 0x0244 */ m3d::ui::ButtonWnd* m_btnRepairList;
        /* 0x0248 */ m3d::ui::Wnd* m_wndRepairPrice;
        /* 0x024c */ m3d::ui::ButtonWnd* m_btnRechargeAll;
        /* 0x0250 */ m3d::ui::ButtonWnd* m_btnRechargeList;
        /* 0x0254 */ m3d::ui::Wnd* m_wndRechargePrice;
        /* 0x0258 */ ref_ptr<CabinList> m_wndCabinsList;
        /* 0x025c */ ref_ptr<BasketList> m_wndBasketsList;
        /* 0x0260 */ ref_ptr<SkinsWnd> m_wndSkinList;
        /* 0x0264 */ ref_ptr<RefuelList> m_wndRefuelList;
        /* 0x0268 */ ref_ptr<RepairList> m_wndRepairList;
        /* 0x026c */ ref_ptr<RechargeList> m_wndRechargeList;
        /* 0x0270 */ ref_ptr<m3d::ui::Wnd> m_curList;
        /* 0x0274 */ m3d::ui::ButtonWnd* m_curListButton;
        /* 0x0278 */ m3d::ui::ButtonWnd* m_curAllButton;
        /* 0x027c */ int32_t m_workshopId;
        /* 0x0280 */ int32_t m_unitsToRepair;
        /* 0x0284 */ int32_t m_maxPossibleUnitsToRepair;
        /* 0x0288 */ int32_t m_repairPrice;
        /* 0x028c */ int32_t m_prevUnitsToRepair;
        /* 0x0290 */ int32_t m_prevMaxPossibleUnitsToRepair;
        /* 0x0294 */ int32_t m_prevRepairPrice;
        /* 0x0298 */ int32_t m_unitsToRefuel;
        /* 0x029c */ int32_t m_maxPossibleUnitsToRefuel;
        /* 0x02a0 */ int32_t m_refuelPrice;
        /* 0x02a4 */ int32_t m_prevUnitsToRefuel;
        /* 0x02a8 */ int32_t m_prevMaxPossibleUnitsToRefuel;
        /* 0x02ac */ int32_t m_prevRefuelPrice;
        /* 0x02b0 */ int32_t m_unitsToRecharge;
        /* 0x02b4 */ int32_t m_maxPossibleUnitsToRecharge;
        /* 0x02b8 */ int32_t m_rechargePrice;
        /* 0x02bc */ int32_t m_prevUnitsToRecharge;
        /* 0x02c0 */ int32_t m_prevMaxPossibleUnitsToRecharge;
        /* 0x02c4 */ int32_t m_prevRechargePrice;
        static GarageWnd::AuxInfo m_aif;
        static m3d::Class m_classGarageWnd;

        int32_t SetupForWorkshop(int32_t);
        int32_t GetWorkshopId() const;
        ai::Workshop* GetWorkshop() const;
        bool IsListOpen() const;
        virtual int32_t GameDataSetup();
        virtual int32_t GameDataUpdate(void*, int32_t);
        virtual int32_t OnWndNotify(m3d::ui::Wnd*, uint32_t, uint32_t, const m3d::AIParam&);
        virtual int32_t OnBeforeAddToWndStation();
        virtual int32_t OnAfterAddToWndStation();
        virtual int32_t OnAfterRemoveFromWndStation();
        void OpenList(ref_ptr<m3d::ui::Wnd>, m3d::ui::ButtonWnd*, m3d::ui::ButtonWnd*, ai::eGameEvent);
        void CloseCurrentList();
        void UpdateServiceActiveState(const vc3::vector<m3d::ui::Wnd *,std::allocator<m3d::ui::Wnd *> >&, bool, ref_ptr<m3d::ui::Wnd>);
        void UpdateActiveStates();
        void UpdateCabinsActiveState();
        void UpdateBasketsActiveState();
        void UpdateNewVehicleActiveState();
        void UpdateSkinActiveState();
        void UpdateRefuelActiveState();
        void UpdateRepairActiveState();
        void UpdateRechargeActiveState();
        void UpdateServiceSelectState(const vc3::vector<m3d::ui::Wnd *,std::allocator<m3d::ui::Wnd *> >&, bool);
        void OnCabins();
        void OnBaskets();
        void OnNewVehicle();
        void OnSkin();
        void OnRefuelAll();
        void OnRefuelList();
        void OnRepairAll();
        void OnRepairList();
        void OnRechargeAll();
        void OnRechargeList();
        int32_t GetUnitsToRefuel() const;
        int32_t GetUnitsToRepair() const;
        int32_t GetUnitsToRecharge() const;
        void GetPossibleRefuel(int32_t&, int32_t&) const;
        void GetPossibleRepair(int32_t&, int32_t&) const;
        void GetPossibleRecharge(int32_t&, int32_t&) const;
        void MaxRefuel();
        void MaxRepair();
        void MaxRecharge();
        void Refuel(int32_t);
        void Repair(int32_t);
        void Recharge(int32_t);
        void FullUpdate();
        ai::GeomRepository* GetWorkshopRepositoryByType(ai::WorkshopRepositoryType) const;
        int32_t GetTownId() const;
        float GetHealthPriceForOneUnit() const;
        int32_t GetFuelPriceForOneUnit() const;
        int32_t GetShellPrice(int32_t) const;
        void UpdateRefuelAllPriceControls(bool);
        void UpdateRepairAllPriceControls(bool);
        void UpdateRechargeAllPriceControls(bool);
        void UpdateAllPriceValues();
        void UpdateAllPricePrevValues();
        void UpdateAllPriceControls(bool);
        void UpdateRepairAllTooltip();
        void UpdateRefuelAllTooltip();
        void UpdateRechargeAllTooltip();
        void OnNewFrame();
        void OnRepositoryChanged(void*);
        void OnPlayerVehicleChanged();
        void OnBuySkin();
        void OnFinishTrade(void*);
        uint32_t EnumColor2Color(help::Color) const;
        GarageWnd();
        GarageWnd(const GarageWnd&);
        virtual ~GarageWnd();
        virtual m3d::Object* Clone();
        virtual m3d::Class* GetClass() const;

        static help::Color GetValueColor(int32_t, int32_t);
        static m3d::Object* CreateObject();
        static m3d::Class* GetBaseClass();
        CUSTOM static void StaticRepair(int32_t units);
        CUSTOM static void StaticRefuel(int32_t units);
    };
    static_assert(sizeof(GarageWnd) == 0x2C8);
}
