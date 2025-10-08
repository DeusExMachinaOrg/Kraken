#pragma once

#include "PhysicBody.h"
#include "SgNode.h"
#include "Numeric.h"
#include "NumericInRangeRegenerating.h"
#include "DecalData.h"
#include "IPriceCoeffProvider.h"

namespace ai
{
	struct VehiclePart : PhysicBody
	{
		struct ModelPart
		{
			float maxHealth;
			float health;
			float jadedEffect;
		};

		struct LoadDecalData
		{
			int node;
			m3d::DecalData dd;
			int meshNum;
		};

		int m_SplashEffect;
		bool m_MakeSplash;
		stable_size_map<int, m3d::SgNode*> m_decals;
		ai::Numeric<float> m_price;
		ai::NumericInRangeRegenerating<float> m_durability;
		CStr m_partName;
		int m_durabilityCoeffsForDamageTypes[4];
		CStr m_blowEffectName;
		stable_size_set<int> m_suppressedLPs;
		stable_size_vector<ai::VehiclePart::ModelPart> m_modelParts;
		CVector m_lastHitPos;
		int m_passToAnotherMapData;
		stable_size_vector<ai::VehiclePart::LoadDecalData> m_loadDecalsData;
		int m_ownerCompoundPart;

		__int32 GetPrice(const ai::IPriceCoeffProvider* provider)
		{
			FUNC(0x006CF0C0, __int32, __thiscall, _GetPrice, VehiclePart*, const ai::IPriceCoeffProvider*);
			return _GetPrice(this, provider);
		}

	};
}