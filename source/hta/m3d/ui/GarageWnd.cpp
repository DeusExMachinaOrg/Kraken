#include "hta/native.hpp"
#include "hta/m3d/ui/GarageWnd.hpp"

namespace hta::m3d::ui {
    NATIVE(0x0044BB40, void GarageWnd::Refuel (int32_t));

    void GarageWnd::StaticRefuel(int32_t units)
    {
        static constexpr auto _Refuel = 0x0044BB40;

        __asm
        {
            xor  ecx, ecx
            push units
            call _Refuel
        }
    }

    void GarageWnd::StaticRepair(int32_t units)
    {
        static constexpr auto _Repair = 0x0044BBD0;

        __asm
        {
            mov eax, units;
            call _Repair;
        }
    }
}