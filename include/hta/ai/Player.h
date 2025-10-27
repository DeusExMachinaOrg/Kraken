#pragma once
#include "hta/Game.h"
#include "hta/ai/Vehicle.h"

namespace ai
{
	struct Player
	{
		inline static Player*& Instance = *(Player**)0x00A135E4;

		ai::Vehicle* GetVehicle()
		{
			FUNC(0x00651190, ai::Vehicle*, __thiscall, _GetVehicle, ai::Player*);
			return _GetVehicle(this);
		}

		int32_t GetMoney() const
		{
			FUNC(0x0064FA80, int32_t, __thiscall, _GetMoney, const ai::Player*);
			return _GetMoney(this);
		}

		int32_t GetSchwarz() const
		{
			FUNC(0x00651380, int32_t, __thiscall, _GetSchwarz, const ai::Player*);
			return _GetSchwarz(this);
		}
	};
}