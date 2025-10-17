#pragma once
#include "hta/m3d/WeatherManager.h"
#include "hta/ai/Vehicle.h"

namespace m3d
{
	struct CWorld
	{
		BYTE _offset[0x39E99C];
		WeatherManager m_weatherManager;

		ai::Vehicle* GetVehicleControlledByPlayer()
		{
			FUNC(0x005C7920, ai::Vehicle*, __thiscall, _GetVehicleControlledByPlayer, CWorld*);
			return _GetVehicleControlledByPlayer(this);
		}
	};
}