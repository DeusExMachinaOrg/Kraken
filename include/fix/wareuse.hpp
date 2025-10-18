#pragma once
#include "config.hpp"
#include "hta/CStr.h"
#include "hta/ai/Vehicle.hpp"
#include "hta/DragDropItemsWnd.h"

namespace kraken::fix::wareuse {
    bool TryRepair(ai::Vehicle* playerVehicle, CStr& name);
    bool TryRefuel(ai::Vehicle* playerVehicle, CStr& name);
    int __fastcall OnMouseButton0Hook(DragDropItemsWnd* dragDropItemsWnd, int, unsigned int state, const PointBase<float>* at);
    void Apply();
}