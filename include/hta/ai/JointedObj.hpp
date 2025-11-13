#pragma once
#include <utils.hpp>

#include "hta/m3d/Class.h"
#include "hta/m3d/Object.h"
#include "hta/ai/Obj.hpp"
#include "hta/m3d/SgNode.hpp"
#include "hta/ai/Event.hpp"
#include "Modifier.h"

class dxSpace;
class dxJoint;
namespace ai {
    struct GeomObj;
    struct ExternalJointInfo;
    struct GoDataForLoad;
    struct JointedObjPrototypeInfo;
}
namespace m3d {
    struct AnimatedModel;
    struct AnimInfo;
}

namespace ai {
    struct JointedObj : ai::Obj // sizeof=0x19C
    {
        protected:
        virtual  ~JointedObj() override /* 0x00 */;

    private:
        JointedObj(const ai::JointedObjPrototypeInfo& prototypeInfo);
        JointedObj(const ai::JointedObj&);
        virtual m3d::Object* Clone() override /* 0x00 */;
        static m3d::Object* __fastcall CreateObject();

    public:
        static m3d::Class* __fastcall GetBaseClass();
        virtual m3d::Class* GetClass() const override /* 0x00 */;
        static m3d::Class m_classJointedObj;
        virtual const ai::PrototypeInfo* GetPrototypeInfo() const /* 0x4c */;
        virtual void Remove() override /* 0x54 */;
        virtual void LoadRuntimeValues(m3d::cmn::XmlFile* xmlFile, const m3d::cmn::XmlNode* xmlNode) override /* 0xb0 */;
        virtual void SaveRuntimeValues(m3d::cmn::XmlFile* xmlFile, m3d::cmn::XmlNode* xmlNode) const override /* 0xb8 */;
        void Init(const CStr& modelName, const CVector& pos, const Quaternion& rot, float mass, m3d::SgNode* toAccept, float strechZ);
        virtual void Update(float elapsedTime, unsigned int workTime) override /* 0x80 */;
        virtual void RenderDebugInfo() const override /* 0xe4 */;
        void Disable(bool disable);
        void InitImpulses(CVector causePos, float force);
        void SetDeadTimer(int resttime, bool testVisibility);
        void SetDisableTimer(int restTime);
        virtual void RelinkSceneGraphNode() override /* 0xe0 */;
        void AddExternalJoint(int IdAttachTo, CVector attachPos, CStr lpName);
        void ReattachExternalJoint(int IdAttachTo);
        virtual void PostCollide() override /* 0x84 */;
        void SetAsRope(bool b);
        void CalcSplineNeighbours();

    protected:
        void PutToNewSpace(dxSpace* Parent);
        virtual void _InternalPostLoad() override /* 0xfc */;
        void RecalcBoundBox();
        bool IsParent(int i, int j);
        void CheckDisablePhysics();

        dxSpace *m_MembersSpace;
        stable_size_vector<ai::GeomObj *> m_Members;
        stable_size_map<unsigned int,ai::GeomObj *> m_ExtraMembers;
        stable_size_vector<dxJoint *> m_Joints;
        stable_size_vector<unsigned int> m_jointsIndices;
        stable_size_vector<dxJoint *> m_externalJoints;
        stable_size_vector<ai::ExternalJointInfo> m_externalJointsInfo;
        bool m_onLoad;
        // padding byte
        // padding byte
        // padding byte
        stable_size_vector<CMatrix> m_JointToGeom;
        CStr m_modelName;
        m3d::SgNode *m_node;
        m3d::AnimatedModel *m_Model;
        m3d::AnimInfo *m_Anim;
        float m_mass;
        float m_strech;
        CVector m_Position;
        bool m_deadTimerActive;
        // padding byte
        // padding byte
        // padding byte
        float m_deadTimer;
        bool m_testVisibility;
        bool m_enabled;
        bool m_disableTimerActive;
        // padding byte
        float m_disableTimer;
        stable_size_vector<ai::GoDataForLoad> m_dataForLoad;
        stable_size_map<unsigned int,ai::GoDataForLoad> m_edataForLoad;
        bool m_asRope;
        // padding byte
        // padding byte
        // padding byte
        //stable_size_map<int,ai::JointedObj::SplineBones> m_splineNeighbours;
    };
}