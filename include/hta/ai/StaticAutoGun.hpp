#pragma once
#include "hta/ai/ComplexPhysicObj.h"


namespace ai {
    class Gun;

    class StaticAutoGunPrototypeInfo : public ai::ComplexPhysicObjPrototypeInfo
    {
    protected:
        virtual void _InternalCopyFrom(const ai::PrototypeInfo& rhs) /* 0x00 */;

    public:
        /* 0x0090 */ float m_maxHealth;
        /* 0x0094 */ CStr m_destroyedModelName;
        StaticAutoGunPrototypeInfo();
        virtual ~StaticAutoGunPrototypeInfo();
        virtual bool LoadFromXML(m3d::cmn::XmlFile* xmlFile, const m3d::cmn::XmlNode* xmlNode) override /* 0x04 */;
        virtual ai::Obj* CreateTargetObject() const /* 0x00 */;
    }; /* size: 0x00a0 */

    static_assert(sizeof(StaticAutoGunPrototypeInfo) == 0x00a0);

    class StaticAutoGun : public ai::ComplexPhysicObj
    {
    protected:
        virtual  ~StaticAutoGun() override /* 0x00 */;

    private:
        StaticAutoGun(const ai::StaticAutoGunPrototypeInfo& prototypeInfo);
        StaticAutoGun(const ai::StaticAutoGun&);
        virtual m3d::Object* Clone() override /* 0x00 */;
        static m3d::Object* __fastcall CreateObject();

    public:
        static m3d::Class* __fastcall GetBaseClass();
        virtual m3d::Class* GetClass() const override /* 0x00 */;
        inline static m3d::Class* m_classStaticAutoGun = (m3d::Class*)0x00A02BFC;
        virtual const ai::StaticAutoGunPrototypeInfo* GetPrototypeInfo() const override /* 0x4c */;

    protected:
        static void __fastcall RegisterProperty(const char* Name, int id, ai::eGObjPropertySaveStatus saveStatus);

    public:
        virtual ai::eGObjPropertySaveStatus GetPropertySaveStatus(int id) const override /* 0x00 */;
        virtual void GetPropertiesNames(stable_size_set<CStr>& Props) const /* 0x00 */;
        virtual void GetPropertiesIDs(stable_size_set<int>& Props) const /* 0x00 */;
        virtual CStr GetPropertyName(int id) const override /* 0x00 */;
        virtual bool SetPropertyById(int propertyId, const m3d::AIParam& newValue) override /* 0x00 */;
        virtual int GetPropertyId(const char* PropertyName) const override /* 0x00 */;

    protected:
        static inline stable_size_map<CStr, int> m_propertiesMap;
        static inline stable_size_map<int, enum ai::eGObjPropertySaveStatus> m_propertiesSaveStatesMap;
        virtual bool _GetPropertyDefaultInternal(int propertyId, m3d::AIParam& retVal) const override /* 0x00 */;
        virtual bool _GetPropertyInternal(int propertyId, m3d::AIParam& retVal) const override /* 0x00 */;

    public:
        virtual void LoadRuntimeValues(m3d::cmn::XmlFile* xmlFile, const m3d::cmn::XmlNode* xmlNode) override /* 0xb0 */;
        virtual void SaveRuntimeValues(m3d::cmn::XmlFile* xmlFile, m3d::cmn::XmlNode* xmlNode) const override /* 0xb8 */;
        virtual void Update(float elapsedTime, unsigned int workTime) override /* 0x00 */;
        ai::Gun* getGun();
        bool PointIsReachableForGun(const CVector& pos, const stable_size_vector<int>& exceptions);
        void FireFromWeapon(bool enable);
        void WeaponLookAtPoint(const CVector& lookAt, float elapsedTime);
        virtual void RenderDebugInfo() const override /* 0xe4 */;
        virtual void AddChild(ai::Obj* pObj) override /* 0x00 */;
        virtual bool RemoveChild(ai::Obj* pObj) override /* 0x00 */;
        virtual void SetPositionSelf(const CVector& pos) override /* 0x00 */;
        const ai::NumericInRange<float>& Health() const;
        ai::NumericInRange<float>& Health();
        const CStr& GetDestroyedModelName() const;
        virtual void InflictDamage(const ai::DamageInfo& damageInfo) override /* 0x00 */;
        virtual bool ApplyModifier(const ai::Modifier& modifier) override /* 0x00 */;
        static void __fastcall Registration();
        float GetMaxFiringRangeAI() const;

    protected:
        virtual void _InternalPostLoad() override /* 0x00 */;
        virtual void _Construct(bool bForAnimation) override /* 0x1bc */;

    private:
        /* 0x014c */ ai::NumericInRange<float> m_health;
        /* 0x01f8 */ ai::NumericInRangeRegenerating<float> m_timeForNextCheck;
        /* 0x02d0 */ CStr m_destroyedModelName;
        /* 0x02dc */ stable_size_set<m3d::Class*> m_targetClasses;
        /* 0x02e8 */ int m_currentEnemyId;
        bool _OnHealthValueBeforeApplyModifier(const ai::Modifier& modifier, float& newHealth);
        void _OnHealthValueAfterChange(float oldHealthValue);
    }; /* size: 0x02ec */

    static_assert(sizeof(StaticAutoGun) == 0x02ec);
}