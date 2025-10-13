#pragma once
#include "RefCountedBase.h"
#include "hta/CStr.h"

namespace m3d
{
	class Object;
	class ExportInfo;

    struct Class /* Size=0x1c */
    {
		const char* m_className = nullptr;
		int m_classSize = 0;
		Object* (* m_fnCreateObject)() = nullptr;
		Class* (* m_fnGetBaseClass)() = nullptr;
		int m_index = 0;
		ExportInfo* m_lExports = nullptr;
		void* m_scriptHandle = nullptr;

		// public:
		bool IsKindOf(char const*) const;
		bool IsKindOf(Class const*) const;
		Object* NewInstance() const;
    };
	ASSERT_SIZE(m3d::Class, 0x1c);

	namespace cmn
	{
		struct XmlFile;
		struct XmlNode;
	}
	class Object : RefCountedBase
	{
	    public:
	        virtual Object* Clone();
	        virtual int ReadFromXmlNode(cmn::XmlFile*, cmn::XmlNode*) = 0;
	        virtual int ReadFromXmlNodeAfterAdd(cmn::XmlFile*, cmn::XmlNode*) = 0;
	        virtual int WriteToXmlNode(cmn::XmlFile*, cmn::XmlNode*) = 0;
	        void SetPersistance(bool);
	        bool GetPersistance() const;
	        virtual int SetProperty(unsigned int propId, void* prop) = 0;
	        virtual int GetProperty(unsigned int propId, void* prop) const = 0;
	        virtual int GetPropertiesList(std::set<size_t>&) const;
	        Object* GetParent() const;
	        Object* GetFirstChild_() const;
	        Object* GetLastChild_() const;
	        Object* GetNextSibling_() const;
	        Object* GetPrevSibling_() const;
	        int GetNumChildren() const;
	        Object* GetChildByName(CStr const&) const;
	        bool IsDirectChild(Object const*) const;
	        bool IsChildOf(Object const*) const;
	        virtual int AddChild(Object*);
	        int LinkChildAtHead(Object*);
	        int LinkChildAtTail(Object*);
	        int UnlinkChild(Object*);
	        virtual int RemoveChild(Object*);
	        int RemoveAllChildren();
	        void MoveChildToFirstPosition(Object*);
	        void MoveChildToLastPosition(Object*);
	        char const* GetName() const;
	        void SetName(CStr const&);
	        void SetChildDirty(bool);
	        bool GetChildDirty() const;

		// protected
		CStr name;
		bool persistant = true;
		bool isChildDirty = false;

		// private
		Object* parent = nullptr;
		Object* firstChild = nullptr;
		Object* lastChild = nullptr;
		Object* nextSibling = nullptr;
		Object* prevSibling = nullptr;
		int numChildren  = 0;
		// public
		void* scriptHandle = nullptr;

        virtual Class* GetClass() const;
        virtual char const* GetClassNameA() const;
        bool IsKindOf(char const*) const;
        bool IsKindOf(Class const*) const;
        static Class* GetBaseClass();
        static Object* CreateObject();

		bool IsKindOf(int cls)
		{
			FUNC(0x006161E0, bool, __thiscall, _IsKindOf, Object*, int);
			return _IsKindOf(this, cls);
		}
	};

	ASSERT_SIZE(m3d::Object, 0x34);
}