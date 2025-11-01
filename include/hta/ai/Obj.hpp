#pragma once
#include <utils.hpp>

#include "hta/m3d/Class.h"
#include "hta/m3d/Object.h"
#include "hta/m3d/SgNode.hpp"
#include "hta/ai/Event.hpp"
#include "Modifier.h"

namespace m3d::cmn {
    struct XmlFile;
    struct XmlNode;
}

namespace ai {
    struct PrototypeInfo;
    struct Obj;
    struct Event;
    struct IPriceCoeffProvider;
    struct AI;
    struct Affix;
    struct DamageInfo;
    struct GeomRepository;

    struct PrototypeInfo { /* Size=0x40 */
        /* 0x0004 */ CStr m_className;
        /* 0x0010 */ CStr m_prototypeName;
        /* 0x001c */ int32_t m_prototypeId;
        /* 0x0020 */ int32_t m_resourceId;
        /* 0x0024 */ bool m_bIsUpdating;
        /* 0x0025 */ bool m_bVisibleInEncyclopedia;
        /* 0x0026 */ bool m_bApplyAffixes;
        /* 0x0028 */ uint32_t m_price;
        /* 0x002c */ bool m_bIsAbstract;
        /* 0x0030 */ CStr m_parentPrototypeName;
        /* 0x003c */ m3d::Class* m_protoClassObject;
        
        PrototypeInfo();
        PrototypeInfo(const PrototypeInfo&);
        virtual ~PrototypeInfo();
        virtual bool LoadFromXML(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual void PostLoad();
        virtual void RefreshFromXml(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        virtual Obj* CreateTargetObject() const;
        bool bIsAbstract() const;
        const CStr& GetParentPrototypeName() const;
        virtual uint32_t GetBasePrice() const;
        void CopyFrom(const PrototypeInfo&);
        CStr GetDebugDescription() const;
        bool IsPrototypeOf(const m3d::Class*) const;
        virtual void _InternalCopyFrom(const PrototypeInfo&);
    };

    static_assert(sizeof(PrototypeInfo) == 0x0040);


    struct Obj : m3d::Object {
        enum HierarchyType : int32_t {
            HIERARCHY_CHILD = 0x0000,
            HIERARCHY_COMPONENT = 0x0001,
        };
        struct ai::Obj::EventRecipientInfo { /* Size=0x14 */
            eGameEvent m_eventId;
            stable_size_vector<int> m_objIds;
        };
        /* Size=0xc0 */
        /* 0x0000: fields for m3d::Object */
        /* 0x0034 */ int32_t m_objId;
        /* 0x0038 */ int32_t m_updatingObjId;
        /* 0x003c */ bool m_bIsUpdating;
        /* 0x003d */ bool m_bMustBeUpdating;
        /* 0x0040 */ uint32_t m_flags;
        /* 0x0044 */ uint32_t m_actionID;
        /* 0x0048 */ uint32_t m_prevActionID;
        /* 0x004c */ float m_timeOut;
        /* 0x0050 */ bool m_bNeedPostLoad;
        /* 0x0051 */ bool m_bMustCreateVisualPart;
        /* 0x0052 */ bool m_bPassedToAnotherMap;
        /* 0x0054 */ int32_t m_belong;
        /* 0x0058 */ int32_t m_parentId;
        /* 0x005c */ GeomRepository* m_parentRepository;
        /* 0x0060 */ int32_t m_LastDamageSource;
        /* 0x0064 */ bool m_bIsAlreadySaved;
        /* 0x0068 */ Obj::HierarchyType m_hierarchyType;
        /* 0x006c */ int32_t m_prototypeId;
        /* 0x0070 */ stable_size_map<int,Obj*> m_allChildren;
        /* 0x007c */ stable_size_vector<int> m_appliedPrefixIds;
        /* 0x008c */ stable_size_vector<int> m_appliedSuffixIds;
        /* 0x009c */ bool m_bAffixesWasApplied;
        /* 0x00a0 */ stable_size_vector<Modifier> m_modifiers;
        /* 0x00b0 */ stable_size_vector<Obj::EventRecipientInfo> m_eventRecipients;
        public: static m3d::Class m_classObj;
        protected: static stable_size_map<CStr,int> m_propertiesMap;
        protected: static stable_size_map<int,enum eGObjPropertySaveStatus> m_propertiesSaveStatesMap;
        public:
            Obj(const Obj&);
            Obj(const PrototypeInfo&);
            Obj();
            virtual ~Obj();
            virtual m3d::Class* GetRtClass() const;
            virtual int32_t OnEvent(const Event& evn);
            int32_t GetId() const;
            bool IsUpdating() const;
            uint32_t GetFlags() const;
            int32_t GetParentId() const;
            int32_t GetPrototypeId() const;
            void SetParentInvalid();
            std::map<int,Obj *,std::less<int>,std::allocator<std::pair<int const ,Obj *> > >& GetChildren();
            void UnlinkFromParent();
            void LinkToParent(int32_t, Obj::HierarchyType);
            bool bHasParent() const;
            void SetName(const CStr&);
            void SetNameFromScript(const CStr&);
            GeomRepository* GetParentRepository() const;
            void SetParentRepository(GeomRepository*);
            virtual AI* GetAIPtr();
            virtual bool NeedCinematicUpdate();
            void AddToCinematic();
            void RemoveFromCinematic();
            virtual const PrototypeInfo* GetPrototypeInfo() const;
            void SetTimeOut(float);
            void StopTimeOut();
            bool GetPassedToAnotherMapStatus() const;
            virtual void SetPassedToAnotherMapStatus();
            void CreateVisualPart();
            void RefreshAfterPassageToAnotherMap();
            virtual void Remove();
            void SetToMap();
            bool TimeOutFinished();
            int32_t TimeOutActivated() const;
            virtual eGObjPropertySaveStatus GetPropertySaveStatus(int32_t) const;
            virtual void GetPropertiesNames(std::set<CStr,std::less<CStr>,std::allocator<CStr> >&) const;
            virtual void GetPropertiesIDs(std::set<int,std::less<int>,std::allocator<int> >&) const;
            virtual int32_t GetProperty(uint32_t, void*) const;
            virtual m3d::AIParam GetProperty(const char*) const;
            virtual m3d::AIParam GetPropertyDefault(const char*) const;
            virtual m3d::AIParam GetPropertyDefaultById(int32_t) const;
            virtual m3d::AIParam GetPropertyById(int32_t) const;
            virtual int32_t GetPropertyId(const char*) const;
            virtual CStr GetPropertyName(int32_t) const;
            int32_t SetProperty(uint32_t, void*);
            bool SetProperty(const char*, const m3d::AIParam&);
            virtual bool SetPropertyById(int32_t, const m3d::AIParam&);
            virtual void Update(float, uint32_t);
            virtual void PostCollide();
            void AddModifier(const Modifier&);
            void AddModifier(const char*, const char*);
            void Send(Obj*, const char*, const char*);
            virtual bool ApplyModifier(const Modifier&);
            bool ApplyAffix(const Affix*);
            void ApplyRandomAffixes(int32_t);
            bool ApplyAffixByName(const char*);
            virtual void InflictDamage(const DamageInfo&);
            CStr GetFullDescriptionWithAffixes() const;
            CStr GetDebugDescription() const;
            void Subscribe(eGameEvent, int32_t);
            void Unsubscribe(eGameEvent, int32_t);
            void CauseEvent(eGameEvent, float, m3d::AIParam, m3d::AIParam) const;
            void ValidateEventRecipientsList();
            virtual Obj* GetChild(int32_t) const;
            virtual int32_t AddChild(m3d::Object*);
            virtual void AddChild(Obj*);
            virtual bool CanChildBeAdded(m3d::Class*) const;
            virtual int32_t RemoveChild(m3d::Object*);
            virtual bool RemoveChild(Obj*);
            virtual void RemoveComponent(Obj*);
            virtual void CreateChildren();
            virtual void Dump() const;
            virtual void LoadFromXML(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
            virtual void LoadRuntimeValues(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
            virtual void SaveToXML(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
            virtual void SaveRuntimeValues(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
            virtual bool bIsEqualToPrototype() const;
            void PostLoad();
            virtual void SetBelong(int32_t);
            int32_t GetBelong() const;
            virtual Obj* GetParent() const;
            bool IsAlive() const;
            bool bIsEnemyWith(const Obj*) const;
            virtual void SetVisible();
            virtual void SetInvisible();
            bool bIsVisible() const;
            virtual void StackOpen();
            virtual void StackClose();
            virtual void StackLoop();
            virtual void TransferPhysicParamsToSceneGraphNode();
            virtual void RelinkSceneGraphNode();
            virtual void RenderDebugInfo() const;
            virtual void ReceiveNodesToLink(stable_size_list<m3d::SgNode *>&) const;
            bool GetDeletedStatus() const;
            virtual uint32_t GetPrice(const IPriceCoeffProvider*) const;
            float GetPriceCoeff(const IPriceCoeffProvider*) const;
            virtual uint32_t GetSchwarz() const;
            virtual Obj* CloneObj();
            virtual void ClearSavedStatus();
            bool IsAffixesApplied() const;
            void SetAffixesApplied(bool);
            void _SetDeadStatus();
            bool _GetDeadStatus() const;
            virtual void _InternalPostLoad();
            virtual void _InternalCreateVisualPart();
            virtual void _SetAllPropertiesToMax();
            virtual bool _GetPropertyInternal(int32_t, m3d::AIParam&) const;
            virtual bool _GetPropertyDefaultInternal(int32_t, m3d::AIParam&) const;
            std::map<int,Obj *,std::less<int>,std::allocator<std::pair<int const ,Obj *> > >& getAllChildren();
            int32_t GetLastDamageSource() const;
            void SetLastDamageSource(int32_t);
            int32_t _GetIndexByEventId(eGameEvent) const;
            void _Init();
            void OnSubscribe(const Event&);
            void OnUnsubscribe(const Event&);
        
            static m3d::Class* GetBaseClass();
            static void Registration();
            static void RegisterProperty(const char*, int32_t, eGObjPropertySaveStatus);
            static m3d::AIParam AIGetCmdParam1(Obj*);
            static m3d::AIParam AIGetCmdParam2(Obj*);
            static m3d::AIParam AIGetCmdParam3(Obj*);
            static m3d::AIParam AIGetState2Param1(Obj*);
            static m3d::AIParam AIGetState2Param2(Obj*);
            static m3d::AIParam AIGetState2Param3(Obj*);
            static m3d::AIParam AIGetState1Param1(Obj*);
            static m3d::AIParam AIGetState1Param2(Obj*);
            static m3d::AIParam AIGetState1Param3(Obj*);
            static m3d::AIParam AIGetMessage2Param1(Obj*);
            static m3d::AIParam AIGetMessage1Param1(Obj*);
            static m3d::AIParam AIGetParentID(Obj*);
            static m3d::AIParam AIGetOwnerID(Obj*);
    };
}