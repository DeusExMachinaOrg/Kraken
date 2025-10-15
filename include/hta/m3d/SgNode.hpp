#pragma once

#include "hta/Quaternion.h"
#include "hta/CVector.h"
#include "hta/CMatrix.hpp"
#include "hta/AABB.hpp"
#include "hta/OBB.hpp"
#include "hta/m3d/Object.h"
#include "hta/m3d/Class.h"
#include "utils.hpp"

namespace cmn {
    struct XmlFile;
    struct XmlNode;
};

namespace m3d {
    struct DataServer;
    struct SceneGraph;
    struct GraphItemsForSgNode;

    enum SgNodeRenderFlags : int32_t {
        NRF_DEFAULT = 0x0000,
        NRF_RMUL_BY_MAT = 0x0001,
        NRF_LMUL_BY_MAT = 0x0002,
        NRF_NO_LIGHTING = 0x0004,
    };

    enum TransparencyType : int32_t {
        TT_NONE = 0x0000,
        TT_VISIBILITY = 0x0001,
        TT_PERMANENT = 0x0002,
    };

    struct TransparencyParams {
        float value;
        float startDist;
        float objectWidth;
    };


    struct SgNode : public Object { /* Size=0x1d4 */
        enum Ritual : int32_t {
            RITUAL_NONE = 0x0000,
            RITUAL_THINK_NODE = 0x0001,
            RITUAL_REGISTERED_NODE = 0x0002,
            RITUAL_THINK_AND_REGISTERED_NODE = 0x0003,
        };
        /* 0x0000: fields for Object */
        /* 0x0034 */ protected: int32_t m_nextThinkTime;
        /* 0x0038 */ protected: int32_t m_prevThinkTime;
        /* 0x003c */ protected: int32_t m_ttl;
        /* 0x0040 */ protected: CMatrix m_ownXForm;
        /* 0x0080 */ protected: CMatrix m_currentXForm;
        /* 0x00c0 */ protected: CVector m_origin;
        /* 0x00cc */ protected: CVector m_scaling;
        /* 0x00d8 */ protected: Quaternion m_rotation;
        /* 0x00e8 */ protected: CVector m_currentWorldOrigin;
        /* 0x00f4 */ protected: Quaternion m_currentWorldRotation;
        /* 0x0104 */ protected: CVector m_originWorldAbsForSphere;
        /* 0x0110 */ protected: float m_boundingRadius;
        /* 0x0114 */ protected: Aabb m_boundingBox;
        /* 0x012c */ protected: Aabb m_ownBoundingBox;
        /* 0x0144 */ protected: bool m_isOriginRelative;
        /* 0x0148 */ protected: uint32_t m_isXFormDirty;
        /* 0x014c */ protected: bool m_isOwnBoundingBoxDirty;
        /* 0x014d */ protected: bool m_removeImmediateAfterParent;
        /* 0x014e */ protected: bool m_isRemoveIfFree;
        /* 0x014f */ protected: bool m_isInRemoveIfFree;
        /* 0x0150 */ protected: bool m_isContoured;
        /* 0x0154 */ protected: uint32_t m_contourColor;
        /* 0x0158 */ protected: float m_contourWidth;
        /* 0x015c */ protected: int32_t m_frameTransparent;
        /* 0x0160 */ protected: TransparencyType m_transparencyType;
        /* 0x0164 */ protected: TransparencyParams m_transparencyParams;
        /* 0x0170 */ protected: bool m_isWaitingForRender;
        /* 0x0174 */ protected: int32_t m_srvId;
        /* 0x0178 */ public: int32_t m_frameVisible;
        /* 0x017c */ public: int32_t m_frameVisible2;
        /* 0x0180 */ public: float m_onScreenSize;
        /* 0x0184 */ public: bool m_isRootNode;
        /* 0x0188 */ protected: int32_t m_properties[3];
        /* 0x0194 */ protected: CStr m_debugMsg;
        /* 0x01a0 */ protected: uint32_t m_props[10];
        /* 0x01c8 */ protected: GraphItemsForSgNode* m_forGraph;
        /* 0x01cc */ public: int32_t m_predictIdx;
        /* 0x01d0 */ private: Ritual m_initedWithRitual;
        public: static Class m_classSgNode;
        
        protected: SgNode();
        protected: SgNode(const SgNode&);
        protected: virtual ~SgNode();
        public: virtual Object* Clone() = 0;
        public: virtual Class* GetClass() const = 0;
        public: virtual int32_t ReadFromXmlNode(cmn::XmlFile*, cmn::XmlNode*);
        public: virtual int32_t ReadFromXmlNodeAfterAdd(cmn::XmlFile*, cmn::XmlNode*);
        public: virtual int32_t WriteToXmlNode(cmn::XmlFile*, cmn::XmlNode*);
        public: virtual int32_t SetProperty(uint32_t, void*);
        public: virtual int32_t GetProperty(uint32_t, void*) const;
        public: virtual int32_t GetPropertiesList(stable_size_set<unsigned int>&) const;
        public: int32_t SetServerItemProperty(uint32_t, void*) const;
        public: int32_t GetServerItemProperty(uint32_t, void*) const;
        public: virtual int32_t AddChild(Object*);
        public: virtual int32_t RemoveChild(Object*);
        public: virtual DataServer* GetServer() const;
        public: int32_t GetTtl() const;
        public: virtual bool IsFree() const;
        public: virtual void CanBeFree();
        public: void RemoveImmediateAfterParent(bool);
        public: void SetBoundingBoxDirty();
        public: virtual void Restart();
        public: void SetTransparencyType(TransparencyType);
        public: int32_t GetNextThinkTime() const;
        public: int32_t GetPrevThinkTime() const;
        public: void SetPrevThinkTime(int32_t);
        public: bool IsXFormUpdateNeeded() const;
        public: int32_t SetOriginRel(const CVector&);
        public: int32_t SetOriginAbs(const CVector&);
        public: int32_t SetScale(const CVector&);
        public: const CVector& GetScale() const;
        public: int32_t SetRotation(const Quaternion&);
        public: const Quaternion& GetRotation() const;
        public: const CVector& GetOriginWorldAbs() const;
        public: const Quaternion& GetRotationWorldAbs() const;
        public: const CVector& GetOriginWorldAbsForSphere() const;
        public: const CVector& GetOrigin() const;
        public: bool GetRelativeFlag() const;
        public: const CMatrix& GetCurrentMatrix() const;
        public: float GetBoundingRadius() const;
        public: Obb GetObb() const;
        public: Aabb GetAabb() const;
        public: Aabb GetOwnAabb() const;
        public: void GetVisCellBounds(PointBase<int>&, PointBase<int>&) const;
        public: bool VisCellBoundsChanged() const;
        public: TransparencyParams& GetTransparencyParams();
        public: virtual float IntersectRay(const CVector&, const CVector&, SgNode*&, Class*);
        public: virtual int32_t UpdateXForm(bool, bool);
        public: virtual int32_t Think(int32_t, int32_t);
        public: virtual int32_t Render(SgNodeRenderFlags, void*, int32_t, int32_t);
        public: SceneGraph* GetGraph();
        public: int32_t GetServerHandle() const;
        public: uint32_t GetContourColor();
        public: float GetContourWidth();
        protected: virtual void UpdateOwnBoundingBox();
        protected: CMatrix MatrixFromFlags(SgNodeRenderFlags, void*) const;
        protected: void RitualInConstructor(Ritual);
        protected: void RitualInDestructor();
        protected: void InternalInit();

        public: static Object* CreateObject();
        public: static Class* GetBaseClass();
    };
};