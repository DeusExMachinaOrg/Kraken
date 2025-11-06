#pragma once

#include "utils.hpp"
#include "hta/m3d/CStrHash.hpp"
#include "hta/m3d/Object.h"
#include "hta/ai/Obj.hpp"
#include "hta/ai/GameTime.hpp"

namespace ai {
    struct ObjContainer : public m3d::Object {
        static inline ObjContainer*& theObjects = *(ObjContainer**)0x00A12E98;
        enum eSAVE_TYPES : int32_t {
            SAVE_LEVEL = 0x0000,
            SAVE_FULL = 0x0001,
            SAVE_EDITOR = 0x0002,
        };

        struct ai::ObjContainer::Node {
            /* Size=0x18 */
            /* 0x0000 */ public: int32_t m_id;
            /* 0x0004 */ public: int32_t m_prevId;
            /* 0x0008 */ public: int32_t m_nextId;
            /* 0x000c */ public: ai::Obj* m_value;
            /* 0x0010 */ public: bool m_isValid;
            /* 0x0014 */ public: int32_t m_totalObjects;
            Node();
        };

        struct ai::ObjContainer::InnerContainer {
            /* Size=0x2c */
            /* 0x0000 */ stable_size_vector<ai::ObjContainer::Node> m_records;
            /* 0x0010 */ stable_size_vector<int> m_freePlaces;
            /* 0x0020 */ uint32_t m_size;
            /* 0x0024 */ int32_t m_firstNodeId;
            /* 0x0028 */ int32_t m_lastNodeId;
        
            InnerContainer(const ai::ObjContainer::InnerContainer&);
            InnerContainer();
            void Clear();
            int32_t Add(ai::Obj*);
            bool AddWithOwnObjId(ai::Obj*, int32_t);
            uint32_t size() const;
            bool empty() const;
            ai::Obj* GetObjById(int32_t);
            void EraseNode(ai::ObjContainer::Node&, bool);
            ai::ObjContainer::Node* _GetNodeById(int32_t);
            ~InnerContainer();
        };

        /* Size=0x120 */
        /* 0x0000: fields for m3d::Object */
        /* 0x0034 */ ObjContainer::InnerContainer m_allObjects;
        /* 0x0060 */ ObjContainer::InnerContainer m_updatingObjects;
        /* 0x008c */ stable_size_map<CStr,int> m_nameToIdMap;
        /* 0x0098 */ ObjContainer::eSAVE_TYPES m_SaveType;
        /* 0x009c */ m3d::CStrHash<CStr> m_ObjectFullNames;
        /* 0x00a8 */ GameTime m_GameTime;
        /* 0x00c0 */ bool m_GameTimePaused;
        /* 0x00c4 */ uint32_t m_denyCreationCount;
        /* 0x00c8 */ int32_t m_numRemovalsLastFrame;
        /* 0x00cc */ stable_size_vector<int> m_objIdsToUpdate;
        /* 0x00dc */ stable_size_vector<int> m_objIdsToNotUpdate;
        /* 0x00ec */ stable_size_vector<Obj*> m_objectsToPostCollide;
        /* 0x00fc */ stable_size_vector<int> m_objIdsToRelinkSceneGraphNode;
        /* 0x010c */ stable_size_vector<int> m_objIdsToRemove;
        /* 0x011c */ bool m_inPurge;
        /* 0x011d */ bool m_inUpdate;
        /* 0x011e */ bool m_bSaveAllowed;

        static m3d::Class m_classObjContainer;
        static const int32_t BITS_IN_MAX_OBJECTS;
        static const int32_t MAX_OBJECTS;
        static const int32_t MAX_OBJECTS_MASK;
        static const int32_t MAX_OBJECTS_IN_CELL;
        
        ObjContainer();
        ObjContainer(const ObjContainer&);
        virtual ~ObjContainer();
        virtual m3d::Object* Clone();
        virtual m3d::Class* GetClass() const;
        uint32_t size() const;
        bool empty() const;
        uint32_t GetNumUpdatingObjects() const;
        void SaveToXml(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
        void Clear(bool);
        void DeleteAll();
        void Purge();
        void Update(float, uint32_t, bool);
        int32_t GetPrototypeId(const char*) const;
        CStr GetPrototypeName(int32_t) const;
        int32_t CreateEntityForLoad(int32_t, const char*, int32_t, int32_t);
        int32_t CreateNewObject(int32_t, const char*, int32_t, int32_t);
        int32_t CreateNewObjectWithSuspendedPostLoad(int32_t, const char*, int32_t, int32_t);
        void MessageBoxA(int32_t, int32_t, Obj*);
        void SetTolerance(int32_t, int32_t, float);
        float GetTolerance(int32_t, int32_t) const;
        void IncTolerance(int32_t, int32_t, float);
        Obj* GetEntityByObjId(int32_t);
        Obj* GetEntityByObjName(const CStr&);
        int32_t GetObjIdByObjName(const CStr&);
        void SetObjName(int32_t, const CStr&);
        CStr GetObjectFullName(const CStr&) const;
        m3d::AIParam GetObjList(const char*, const CVector&, float) const;
        bool AddWithOwnObjId(Obj*);
        void AddObjToPostCollideList(Obj*);
        void PostCollide();
        void LoadObjectNamesFromXML(const CStr&);
        void SetGameTimeInt64(int64_t);
        int64_t GetGameTimeInt64() const;
        float GetGameTimeDiff() const;
        void SetGameTime(int32_t, int32_t, int32_t, int32_t, int32_t);
        //m3d::AIParam GetGameTime() const;
        //m3d::AIParam Get24HourTime() const;
        void PauseGameTime();
        void UnpauseGameTime();
        void SaveNodeStatesToXml(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
        void LoadNodeStatesFromXml(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*);
        void TransferPhysicParamsToSceneGraph();
        void RelinkSceneGraphNodes();
        void LinkGeomsToCollisionCells();
        void UnlinkGeomsFromCollisionCells();
        void RelinkGeomsToCollisionCells();
        int32_t GetNumRemovalsLastFrame() const;
        void PassToMap(const CStr&, const CStr&, int32_t, bool);
        float GetHeight(float, float) const;
        void AddObjToUpdate(Obj*);
        void AddObjToNotUpdate(Obj*);
        void AddObjIdToRelinkSceneGraphNode(int32_t);
        void AddObjIdToRemove(int32_t);
        void DumpPhysicInfo(const CStr&) const;
        void Dump();
        GameTime& getGameTime();
        const GameTime& getGameTime() const;
        void DenyCreation();
        void PermitCreation();
        void AllowSave(bool);
        bool IsSaveAllowed() const;
        int32_t _Add(Obj*);
        void _DeleteObj(Obj*&);
        void _PassToMapAfterFading();
        void _SetObjUpdating(int32_t);
        void _SetObjNotUpdating(int32_t);

        static m3d::Object* CreateObject();
        static m3d::Class* GetBaseClass();

        m3d::AIParam GetGameTime() const
        {
            FUNC(0x00631C50, m3d::AIParam, __thiscall, _GetGameTime, const ObjContainer*);
            return _GetGameTime(this);
        }

        m3d::AIParam Get24HourTime() const
        {
    		FUNC(0x00631C70, m3d::AIParam, __thiscall, _Get24HourTime, const ObjContainer*);
    		return _Get24HourTime(this);
        }


    };
    ASSERT_SIZE(ObjContainer, 0x0120);
}
