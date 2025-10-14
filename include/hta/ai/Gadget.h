#include "Obj.h"
#include "VehiclePart.h"
#include "Vehicle.h"
#include "Gun.h"

namespace ai
{
    struct GadgetPrototypeInfo : PrototypeInfo
    {
        // public:
        enum GadgetAppliers
        {
            GA_VEHICLE = 0x0,
            GA_OBJECT_BY_RESOURCE = 0x1,
            GA_GUN_BY_TYPE = 0x2,
        };

        struct GadgetApplicationInfo
        {
            // public:
            GadgetApplicationInfo();

            // private:
            GadgetAppliers applierType;
            int targetResourceId;
            ai::FiringTypes targetFiringType;
        };

        struct ModificationInfo
        {
            // public:
            enum ModificationType
            {
                MULTIPLY = 0x0,
                ADD = 0x1,
            };

            // public:
            bool ApplyToObj(ai::Obj*, bool) const;
            ModificationInfo(CStr const&, ai::GadgetPrototypeInfo const*);

            // private:
            GadgetApplicationInfo m_applierInfo;
            CStr m_propertyName;
            ModificationType m_modificationType;
            m3d::AIParam m_value;
        };

        // public:
        virtual ai::Obj* CreateTargetObject() const;
        bool ApplyToVp(ai::VehiclePart*, bool) const;
        GadgetPrototypeInfo();
        int GetSkinNum() const;
        CStr const& GetModelName() const;
        virtual bool ApplyToVehicle(ai::Vehicle*, bool) const;
        virtual bool LoadFromXML(m3d::cmn::XmlFile*, m3d::cmn::XmlNode const*);
        std::vector<ModificationInfo, std::allocator<ModificationInfo> > const& GetModifications() const;

        // private:
        std::vector<ai::GadgetPrototypeInfo::ModificationInfo> m_modifications;
        CStr m_modelName;
        int m_skinNum;
    };

    struct Gadget : Obj
    {
        virtual int GetPropertyId(char const *) const ;
        int GetSlotNum() const ;
        void SetSlotNum(int);
        virtual GadgetPrototypeInfo const * GetPrototypeInfo() const ;
        Gadget(GadgetPrototypeInfo const &);
        bool ApplyToVehicle(Vehicle *,bool) const ;
        virtual bool SetPropertyById(int,m3d::AIParam const &);
        static m3d::Class * GetBaseClass();
        bool ApplyToVp(VehiclePart *,bool) const ;
        virtual void GetPropertiesNames(std::set<CStr,std::less<CStr>,std::allocator<CStr> > &) const ;
        virtual eGObjPropertySaveStatus GetPropertySaveStatus(int) const ;
        virtual CStr GetPropertyName(int) const ;
        virtual m3d::Class * GetClass() const ;
        static void __fastcall Registration();
        virtual void GetPropertiesIDs(std::set<int,std::less<int>,std::allocator<int> > &) const ;

        // protected:
        virtual bool _GetPropertyInternal(int,m3d::AIParam &) const ;
        virtual bool _GetPropertyDefaultInternal(int,m3d::AIParam &) const ;
        virtual ~Gadget();
        static void __fastcall RegisterProperty(char const *,int,eGObjPropertySaveStatus);

        // private:
        virtual m3d::Object * Clone();
        static m3d::Object * CreateObject();

        // private:
        int m_slotNum;
        // public: 
        static m3d::Class m_classGadget;
        // protected: 
        static std::map<CStr,int,ai::Obj::LessNoCaseCStr > m_propertiesMap;
        static std::map<int,enum ai::eGObjPropertySaveStatus,std::less<int> > m_propertiesSaveStatesMap;
    
        const GadgetPrototypeInfo* GetPrototypeInfo()
        {
			FUNC(0x006DBA20, const GadgetPrototypeInfo*, __thiscall, _GetPrototypeInfo, Gadget*);
			return _GetPrototypeInfo(this);
        };
    };
    
    ASSERT_SIZE(ai::Gadget, 0xc4);
}
