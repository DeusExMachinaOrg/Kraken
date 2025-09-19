#pragma once
#include "FlatLine.h"

namespace ai
{
    struct DrivingValues
    {
        FlatLine checkLine;
        float checkCircleRadius;
        float nextAngle;
        float brakingCircleRadius;
    };
}