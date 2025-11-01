#pragma once

#include "CStr.h"
#include "ai/Obj.h"
#include "ai/PhysicObj.h"
#include "ai/VehiclePart.h"
#include "containers.hpp"
#include "Obj.h"
#include "hta/m3d/Object.h"

#include "stdafx.hpp"

namespace ai {
    struct dxSpace; // "ode/ode.hpp" in headers
    struct CompoundVehiclePartPrototypeInfo : VehiclePartPrototypeInfo {
        struct TPartInfo {
            /* Size=0x14 */
            /* 0x0000 */ int32_t prototypeId;
            /* 0x0004 */ CStr prototypeName;
            /* 0x0010 */ uint32_t index;

            TPartInfo(const TPartInfo&);
            TPartInfo();
            ~TPartInfo();
        };
        static_assert(sizeof(TPartInfo) == 0x14);
        /* Size=0x11c */
        /* 0x0000: fields for VehiclePartPrototypeInfo */
        /* 0x0110 */ stable_size_map<CStr, TPartInfo> m_PartInfo;

        CompoundVehiclePartPrototypeInfo();
        virtual bool LoadFromXML(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual void PostLoad();
        virtual ~CompoundVehiclePartPrototypeInfo();
    };
    static_assert(sizeof(CompoundVehiclePartPrototypeInfo) == 0x11c);

    struct CompoundVehiclePart : public VehiclePart {
        struct TVehiclePart {
            /* Size=0x8 */
            /* 0x0000 */ VehiclePart* vp;
            /* 0x0004 */ uint32_t index;

            TVehiclePart(VehiclePart*, uint32_t);
            TVehiclePart();
        };
        static_assert(sizeof(TVehiclePart) == 0x8);
        /* Size=0x2d4 */
        /* 0x0000: fields for VehiclePart */
        /* 0x02c8 */ stable_size_map<CStr, TVehiclePart> m_vehicleParts;
        static m3d::Class m_classCompoundVehiclePart;

        virtual ~CompoundVehiclePart() = 0;
        CompoundVehiclePart(const CompoundVehiclePartPrototypeInfo&);
        CompoundVehiclePart(const CompoundVehiclePart&);
        virtual m3d::Object* Clone();
        virtual m3d::Class* GetClass() const;
        virtual const CompoundVehiclePartPrototypeInfo* GetPrototypeInfo() const;
        virtual void SetVisible();
        virtual void SetInvisible();
        virtual void SetBelong(int32_t);
        virtual void Remove();
        virtual void SetPassedToAnotherMapStatus();
        virtual void CreateChildren();
        virtual void SetOwner(PhysicObj*);
        virtual void RelinkToSpace(dxSpace*);
        virtual float GetMass() const;
        // float GetDurability() const;
        // float GetMaxDurability() const;
        void SetDurability(float);
        void RegenerateDurability(float);
        void SetDurabilityRegeneration(float);
        virtual uint32_t GetPrice(const IPriceCoeffProvider*) const;
        virtual void _InternalCreateVisualPart();
        virtual void _InternalPostLoad();
        std::_Tree<std::_Tmap_traits<CStr, TVehiclePart, std::less<CStr>, std::allocator<std::pair<CStr const, TVehiclePart>>, 0>>::const_iterator begin() const;
        std::_Tree<std::_Tmap_traits<CStr, TVehiclePart, std::less<CStr>, std::allocator<std::pair<CStr const, TVehiclePart>>, 0>>::iterator begin();
        std::_Tree<std::_Tmap_traits<CStr, TVehiclePart, std::less<CStr>, std::allocator<std::pair<CStr const, TVehiclePart>>, 0>>::const_iterator end() const;
        std::_Tree<std::_Tmap_traits<CStr, TVehiclePart, std::less<CStr>, std::allocator<std::pair<CStr const, TVehiclePart>>, 0>>::iterator end();
        std::_Tree<std::_Tmap_traits<CStr, TVehiclePart, std::less<CStr>, std::allocator<std::pair<CStr const, TVehiclePart>>, 0>>::const_iterator find(const CStr&) const;
        std::_Tree<std::_Tmap_traits<CStr, TVehiclePart, std::less<CStr>, std::allocator<std::pair<CStr const, TVehiclePart>>, 0>>::iterator find(const CStr&);
        virtual void LoadFromXML(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual void SaveToXML(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
        virtual void AddChild(Obj*);
        virtual bool RemoveChild(Obj*);
        virtual bool CanChildBeAdded(m3d::Class*) const;
        virtual void RenderDebugInfo() const;
        virtual void ClearSavedStatus();

        static m3d::Object* CreateObject();
        static m3d::Class* GetBaseClass();

        float GetDurability() const
        {
            FUNC(0x006F50F0, float, __thiscall, _GetDurability, const CompoundVehiclePart*);
            return _GetDurability(this);
        }

        float GetMaxDurability() const
        {
            FUNC(0x006F5260, float, __thiscall, _GetMaxDurability, const CompoundVehiclePart*);
            return _GetMaxDurability(this);
        }
    };
    static_assert(sizeof(CompoundVehiclePart) == 0x2d4);
};