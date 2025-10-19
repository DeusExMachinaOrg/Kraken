#pragma once

#include "PhysicBody.h"
#include "SgNode.h"
#include "Numeric.h"
#include "NumericInRangeRegenerating.h"
#include "DecalData.h"
#include "IPriceCoeffProvider.h"

namespace ai
{
    struct TriMesh;
	struct Box;

    struct VehiclePartPrototypeInfo : PhysicBodyPrototypeInfo
    {
	    // public:
        CVector const& GetSize() const;
        VehiclePartPrototypeInfo();
        virtual Obj* CreateTargetObject() const;
        virtual void RefreshFromXml(m3d::cmn::XmlFile*, m3d::cmn::XmlNode const*);
        // virtual ~VehiclePartPrototypeInfo();
        virtual bool LoadFromXML(m3d::cmn::XmlFile*, m3d::cmn::XmlNode const*);

	    // private:
	    void _InitModelMeshes(m3d::cmn::XmlFile*, m3d::cmn::XmlNode const*);
	    int m_weaponPrototypeId;
	    float m_durabilityCoeffsForDamageTypes[4];
	    float m_durability;
	    std::set<CStr> m_loadPoints;
	    CStr m_blowEffectName;
	    bool m_canBeUsedInAutogenerating;
	    float m_repairCoef;
	    std::vector<TriMesh*> m_modelMeshes;
	    std::vector<Box*> m_boundsForMeshes;
	    std::vector<void*> m_verts;
	    std::vector<int*> m_inds;
	    std::vector<int> m_numsTris;
	    std::vector<int> m_vertsStride;
	    std::vector<float> m_groupHealthes;
    };

	ASSERT_SIZE(ai::VehiclePartPrototypeInfo, 0x110);

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

		const ai::VehiclePartPrototypeInfo* GetPrototypeInfo()
		{
			FUNC(0x006D3370, const ai::VehiclePartPrototypeInfo*, __thiscall, _GetPrototypeInfo, VehiclePart*);
			return _GetPrototypeInfo(this);
		}

	};

	ASSERT_SIZE(ai::VehiclePart, 0x2c8);
}