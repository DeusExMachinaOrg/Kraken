#pragma once

namespace ai
{
	struct Obj;

	struct IPriceCoeffProvider { /* Size=0x4 */ 
        virtual ~IPriceCoeffProvider() = default;
        virtual float GetPriceCoeffForObj(ai::Obj const*) const = 0;
	};
}