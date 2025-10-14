#pragma once
#include "hta/m3d/Object.h"
#include "CMatrix.h"

namespace m3d
{
    struct Aabb { /* Size=0x18 */
		float m_box[6];
        // public
        void Create(const CVector&, const CVector&);
        void Offset(const CVector&);
        void Scale(const CVector&);
        float GetSz() const;
        float GetSy() const;
        float GetSx() const;
        void Inflate(float);
        void StartEmbracing();
        void EmbracePoint(const CVector&);
        void EmbraceBox(const Aabb&);
        CVector Min() const;
        CVector Max() const;
        float MaximumComponent() const;
        bool IsPtInside(const CVector&) const;
        bool IsPtInside2(const CVector&) const;
        void Draw(long);
        };

    struct Obb { /* Size=0x48 */
        void Create(Aabb const&, CMatrix const&, bool);
        void Create(CVector const&, CVector const&, CMatrix const&, bool);
        void Draw(unsigned int);
        int IsPtInside2(CVector const&) const;
        Aabb GetBounds() const;
        CVector toLocalRotate(CVector const&) const;
        float IntersectRay(CVector const&, CVector const&) const;
        CVector toWorld(CVector const&) const;
        int IsPtInside(CVector const&) const;

        // private
        CVector m_origin;
        CVector m_basis[3];
        CVector m_min;
        CVector m_max;
    };

    enum TransparencyType
    {
        TT_NONE = 0x0,
        TT_VISIBILITY = 0x1,
        TT_PERMANENT = 0x2,
    };


    struct TransparencyParams
    {
        float value;
        float startDist;
        float objectWidth;
    };

    enum SgNodeRenderFlags
    {
        NRF_DEFAULT = 0x0,
        NRF_RMUL_BY_MAT = 0x1,
        NRF_LMUL_BY_MAT = 0x2,
        NRF_NO_LIGHTING = 0x4,
    };

	struct GraphItemsForSgNode;
	struct SceneGraph;
	struct DataServer;

	struct SgNode : Object
	{
        enum Ritual
        {
            RITUAL_NONE = 0x0,
            RITUAL_THINK_NODE = 0x1,
            RITUAL_REGISTERED_NODE = 0x2,
            RITUAL_THINK_AND_REGISTERED_NODE = 0x3,
        };

		// protected
  		int m_nextThinkTime;
  		int m_prevThinkTime;
  		int m_ttl;
  		CMatrix m_ownXForm;
  		CMatrix m_currentXForm;
  		CVector m_origin;
  		CVector m_scaling;
  		Quaternion m_rotation;
  		CVector m_currentWorldOrigin;
  		Quaternion m_currentWorldRotation;
  		CVector m_originWorldAbsForSphere;
  		float m_boundingRadius;
  		Aabb m_boundingBox;
  		Aabb m_ownBoundingBox;
  		bool m_isOriginRelative;
  		uint32_t m_isXFormDirty;
  		bool m_isOwnBoundingBoxDirty;
  		bool m_removeImmediateAfterParent;
  		bool m_isRemoveIfFree;
  		bool m_isInRemoveIfFree;
  		bool m_isContoured;
  		uint32_t m_contourColor;
  		float m_contourWidth;
  		int m_frameTransparent;
  		m3d::TransparencyType m_transparencyType;
  		m3d::TransparencyParams m_transparencyParams;
  		bool m_isWaitingForRender;
  		int m_srvId;
		// public
  		int m_frameVisible;
  		int m_frameVisible2;
  		float m_onScreenSize;
  		bool m_isRootNode;
		// protected
  		int m_properties[3];
  		CStr m_debugMsg;
  		uint32_t m_props[10];
  		m3d::GraphItemsForSgNode* m_forGraph;
  		// public 
		int m_predictIdx;
  		// private
		m3d::SgNode::Ritual m_initedWithRitual;

  		// public
		virtual m3d::Object* Clone();
  		virtual m3d::Class* GetClass() const;
  		virtual int ReadFromXmlNode(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*);
  		virtual int ReadFromXmlNodeAfterAdd(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*);
  		virtual int WriteToXmlNode(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*);
  		virtual int SetProperty(uint32_t, void*);
  		virtual int GetProperty(uint32_t, void*) const;
  		virtual int GetPropertiesList(std::set<unsigned int,std::less<unsigned int>,std::allocator<unsigned int> >&) const;
  		int SetServerItemProperty(uint32_t, void*) const;
  		int GetServerItemProperty(uint32_t, void*) const;
  		virtual int AddChild(m3d::Object*);
  		virtual int RemoveChild(m3d::Object*);
  		virtual m3d::DataServer* GetServer() const;
  		int GetTtl() const;
  		virtual bool IsFree() const;
  		virtual void CanBeFree();
  		void RemoveImmediateAfterParent(bool);
  		void SetBoundingBoxDirty();
  		virtual void Restart();
  		void SetTransparencyType(m3d::TransparencyType);
  		int GetNextThinkTime() const;
  		int GetPrevThinkTime() const;
  		void SetPrevThinkTime(int);
  		bool IsXFormUpdateNeeded() const;
  		int SetOriginRel(const CVector&);
  		int SetOriginAbs(const CVector&);
  		int SetScale(const CVector&);
  		const CVector& GetScale() const;
  		int SetRotation(const Quaternion&);
  		const Quaternion& GetRotation() const;
  		const CVector& GetOriginWorldAbs() const;
  		const Quaternion& GetRotationWorldAbs() const;
  		const CVector& GetOriginWorldAbsForSphere() const;
  		const CVector& GetOrigin() const;
  		bool GetRelativeFlag() const;
  		const CMatrix& GetCurrentMatrix() const;
  		float GetBoundingRadius() const;
  		Obb GetObb() const;
  		Aabb GetAabb() const;
  		Aabb GetOwnAabb() const;
  		void GetVisCellBounds(PointBase<int>&, PointBase<int>&) const;
  		bool VisCellBoundsChanged() const;
  		m3d::TransparencyParams& GetTransparencyParams();
  		virtual float IntersectRay(const CVector&, const CVector&, m3d::SgNode*&, m3d::Class*);
  		virtual int UpdateXForm(bool, bool);
  		virtual int Think(int, int);
  		virtual int Render(m3d::SgNodeRenderFlags, void*, int, int);
  		m3d::SceneGraph* GetGraph();
  		int GetServerHandle() const;
  		uint32_t GetContourColor();
  		float GetContourWidth();

  		// protected:
		virtual void UpdateOwnBoundingBox();
  		CMatrix MatrixFromFlags(m3d::SgNodeRenderFlags, void*) const;
  		void RitualInConstructor(m3d::SgNode::Ritual);
  		void RitualInDestructor();
  		void InternalInit();

  		public: static m3d::Object* CreateObject();
  		public: static m3d::Class* GetBaseClass();

		// virtual void Dtor() = 0;
		// virtual Object* Clone() = 0;
		// virtual int ReadFromXmlNode() = 0;
		// virtual int ReadFromXmlNodeAfterAdd() = 0;
		// virtual int WriteToXmlNode() = 0;
		// virtual int SetProperty(unsigned int propId, void* prop) = 0;
		// virtual int GetProperty(unsigned int propId, void* prop) = 0;

		void SetRotation(const Quaternion* quat)
		{
			FUNC(0x0040C1B0, void, __thiscall, _SetRotation, m3d::SgNode*, const Quaternion*);
			_SetRotation(this, quat);
		}

		Quaternion* GetRotation()
		{
			FUNC(0x00616D80, Quaternion*, __thiscall, _GetRotation, m3d::SgNode*);
			return _GetRotation(this);
		}
	};

	ASSERT_SIZE(m3d::SgNode, 0x1D4);
}