#pragma once
#include "RefCountedBase.h"
#include "hta/m3d/Class.h"
#include "hta/CStr.h"
#include "utils.hpp"


namespace m3d::cmn {
    struct XmlFile;
    struct XmlNode;
}

namespace m3d
{
	struct Object;

	struct Object : RefCountedBase {
		public:
		/* Size=0x34 */
		/* 0x0000: fields for RefCountedBase */
		/* 0x0008 */ CStr m_name; // protected:
		/* 0x0014 */ protected: bool m_persistant;
		/* 0x0015 */ protected: bool m_isChildDirty;
		/* 0x0018 */ private: Object* m_parent;
		/* 0x001c */ private: Object* m_firstChild;
		/* 0x0020 */ private: Object* m_lastChild;
		/* 0x0024 */ private: Object* m_nextSibling;
		/* 0x0028 */ private: Object* m_prevSibling;
		/* 0x002c */ private: int32_t m_numChildren;
		/* 0x0030 */ public: void* m_scriptHandle;
		static Class m_classObject;
		
		virtual Object* Clone();
		virtual int32_t ReadFromXmlNode(cmn::XmlFile*, cmn::XmlNode*);
		virtual int32_t ReadFromXmlNodeAfterAdd(cmn::XmlFile*, cmn::XmlNode*);
		virtual int32_t WriteToXmlNode(cmn::XmlFile*, cmn::XmlNode*);
		bool GetPersistance() const;
		void SetPersistance(bool);
		virtual int32_t SetProperty(uint32_t, void*);
		virtual int32_t GetProperty(uint32_t, void*) const;
		virtual int32_t GetPropertiesList(stable_size_set<unsigned int>&) const;
		Object* GetParent() const;
		Object* GetFirstChild() const;
		Object* GetLastChild() const;
		Object* GetNextSibling() const;
		Object* GetPrevSibling() const;
		int32_t GetNumChildren() const;
		Object* GetChildByName(const CStr&) const;
		bool IsDirectChild(const Object*) const;
		bool IsChildOf(const Object*) const;
		virtual int32_t AddChild(Object*);
		int32_t LinkChildAtHead(Object*);
		int32_t LinkChildAtTail(Object*);
		int32_t UnlinkChild(Object*);
		virtual int32_t RemoveChild(Object*);
		int32_t RemoveAllChildren();
		void MoveChildToFirstPosition(Object*);
		void MoveChildToLastPosition(Object*);
		const char* GetName() const;
		void SetName(const CStr&);
		void SetChildDirty(bool);
		bool GetChildDirty() const;
		Object* ChildNodeFromXmlNode(cmn::XmlFile*, cmn::XmlNode*);
		Object* ChildNodeFromXmlFile(const char*);
		virtual int32_t IncWeakRef();
		virtual int32_t DecWeakRef();
		virtual int32_t GetWeakRefCount();
		virtual Class* GetClass() const;
		virtual const char* GetClassNameA() const;
		bool IsKindOf(const char*) const;
		bool IsKindOf(const Class* cls) const
		{
			FUNC(0x006161E0, bool, __thiscall, _IsKindOf, const Object*, const Class*);
			return _IsKindOf(this, cls);
		}
		Object(const Object&);
		Object();
		virtual ~Object();
		
		static Class* GetBaseClass();
		static Object* CreateObject();
	};
}