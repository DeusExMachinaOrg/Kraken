#pragma once

#include "hta/m3d/ui/Wnd.hpp"
#include "hta/ai/Vehicle.hpp"

namespace hta::m3d::ui {
    struct ChildPanel : public Wnd {
        /* 0x0220 */ int32_t m_vehicleId;
        static m3d::Class m_classChildPanel;

        virtual void SetVehicleId(int32_t);
        int32_t GetVehicleId() const;
        ai::Vehicle* GetVehicle() const;
        virtual void OnRestoreStyles();
        virtual int32_t GameDataUpdate(void*, int32_t);
        virtual int32_t GameDataClear(bool);
        virtual int32_t OnWndNotify(m3d::ui::Wnd*, uint32_t, uint32_t, const m3d::AIParam&);
        virtual void OnExit();
        ChildPanel();
        ChildPanel(const ChildPanel&);
        virtual ~ChildPanel();
        virtual m3d::Object* Clone();
        virtual m3d::Class* GetClass() const;

        static m3d::Object* CreateObject();
        static m3d::Class* GetBaseClass();
    };
    static_assert(sizeof(ChildPanel) == 0x224);
};