#pragma once
#include "hta/ai/Obj.hpp"
#include "utils.hpp"

namespace ai {
    struct Vehicle;

    enum FiringTypes : int32_t {
        FT_MACHINE_GUN = 0x0000,
        FT_CANNON = 0x0001,
        FT_SHOT_GUN = 0x0002,
        FT_LASER = 0x0003,
        FT_PLASMA = 0x0004,
        FT_ROCKET = 0x0005,
        FT_ARTILLERY = 0x0006,
        FT_THUNDERBOLT = 0x0007,
        FT_MINE = 0x0008,
        FT_NAIL = 0x0009,
        FT_TURBO = 0x000a,
        FT_OIL = 0x000b,
        FT_SMOKE = 0x000c,
        FT_NUM_FIRING_TYPES = 0x000d,
    };

    class GadgetPrototypeInfo : public PrototypeInfo {
        enum GadgetAppliers : int32_t {
            GA_VEHICLE = 0x0000,
            GA_OBJECT_BY_RESOURCE = 0x0001,
            GA_GUN_BY_TYPE = 0x0002,
        };
        enum ModificationType : int32_t {
            MULTIPLY = 0x0000,
            ADD = 0x0001,
        };

        struct GadgetApplicationInfo {
            /* Size=0xc */
            /* 0x0000 */ GadgetPrototypeInfo::GadgetAppliers applierType;
            /* 0x0004 */ int32_t targetResourceId;
            /* 0x0008 */ FiringTypes targetFiringType;
        };

        struct ModificationInfo {
            /* Size=0x38 */
            /* 0x0000 */ public: GadgetApplicationInfo m_applierInfo;
            /* 0x000c */ public: CStr m_propertyName;
            /* 0x0018 */ public: ModificationType m_modificationType;
            /* 0x001c */ public: m3d::AIParam m_value;
        
            ModificationInfo(const GadgetPrototypeInfo::ModificationInfo&);
            ModificationInfo(const CStr&, const GadgetPrototypeInfo*);
            bool ApplyToObj(Obj*, bool) const;
            ~ModificationInfo();
        };

        /* Size=0x60 */
        /* 0x0000: fields for PrototypeInfo */
        /* 0x0040 */ stable_size_vector<ModificationInfo> m_modifications;
        /* 0x0050 */ CStr m_modelName;
        /* 0x005c */ int32_t m_skinNum;
        
        GadgetPrototypeInfo();
        virtual bool LoadFromXML(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual Obj* CreateTargetObject() const;
        virtual bool ApplyToVehicle(Vehicle*, bool) const;
        bool ApplyToVp(VehiclePart*, bool) const;
        const CStr& GetModelName() const;
        int32_t GetSkinNum() const;
        const stable_size_vector<ModificationInfo>& GetModifications() const;
        virtual ~GadgetPrototypeInfo();
    };

    class Gadget : public Obj {
        /* Size=0xc4 */
        /* 0x0000: fields for Obj */
        /* 0x00c0 */ int32_t m_slotNum;

        static m3d::Class m_classGadget;
        static stable_size_map<CStr,int> m_propertiesMap;
        static stable_size_map<int,enum eGObjPropertySaveStatus> m_propertiesSaveStatesMap;

        virtual ~Gadget();
        Gadget(const GadgetPrototypeInfo&);
        Gadget(const Gadget&);
        virtual m3d::Object* Clone();
        virtual m3d::Class* GetClass() const;
        virtual const GadgetPrototypeInfo* GetPrototypeInfo() const;
        virtual eGObjPropertySaveStatus GetPropertySaveStatus(int32_t) const;
        virtual void GetPropertiesNames(stable_size_set<CStr>&) const;
        virtual void GetPropertiesIDs(stable_size_set<int>&) const;
        virtual CStr GetPropertyName(int32_t) const;
        virtual bool SetPropertyById(int32_t, const m3d::AIParam&);
        virtual int32_t GetPropertyId(const char*) const;
        virtual bool _GetPropertyDefaultInternal(int32_t, m3d::AIParam&) const;
        virtual bool _GetPropertyInternal(int32_t, m3d::AIParam&) const;
        int32_t GetSlotNum() const;
        void SetSlotNum(int32_t);
        bool ApplyToVehicle(Vehicle*, bool) const;
        bool ApplyToVp(VehiclePart*, bool) const;

        static m3d::Object* CreateObject();
        static m3d::Class* GetBaseClass();
        static void RegisterProperty(const char*, int32_t, eGObjPropertySaveStatus);
        static void Registration();
    };
};