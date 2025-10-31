#pragma once
#include "RefCountedBase.h"
#include "hta/CStr.h"
#include <stdint.h>

namespace m3d
{
	namespace cmn
	{
		struct XmlFile;
		struct XmlNode;
	}

	struct Object;

    enum eExportType
    {
        METHOD = 0x0,
        NATIVE_METHOD = 0x1,
    };

    struct ExportInfo
    {
        const char* name = nullptr;
        eExportType type = METHOD;
        void* addr1 = nullptr;
        void* addr2 = nullptr;
        const char* returns = nullptr;
        const char* params = nullptr;
        const char* desc = nullptr;
    };

    struct Class {
        /* Size=0x1c */
        /* 0x0000 */ const char* m_className;
        /* 0x0004 */ int32_t m_classSize;
        /* 0x0008 */ Object* (* m_fnCreateObject)();
        /* 0x000c */ Class* (* m_fnGetBaseClass)();
        /* 0x0010 */ int32_t m_index;
        /* 0x0014 */ ExportInfo* m_lExports;
        /* 0x0018 */ void* m_scriptHandle;
        
        bool IsKindOf(const char*) const;
        bool IsKindOf(const Class*) const;
        Object* NewInstance() const;
    };
	ASSERT_SIZE(m3d::Class, 0x1c);

	struct Object : RefCountedBase
	{
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

	    // protected:
        Object* ChildNodeFromXmlNode(cmn::XmlFile*, cmn::XmlNode*);

        Object* ChildNodeFromXmlFile(char const*);
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