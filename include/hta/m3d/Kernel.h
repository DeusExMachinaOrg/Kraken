#pragma once
#include "stdafx.hpp"
#include "EngineConfig.h"
#include "ScriptServer.h"
#include "Timer.h"
#include "Object.h"

namespace m3d
{
    namespace fs {
        class FileServer;
    }
    namespace cmn {
        class IniFile;
        class XmlFile;
    }
    struct Class;
    struct Kernel
    {
        inline static Kernel*& g_Kernel = *(Kernel**)0x00A0988C;

        virtual  ~Kernel() /* 0x00 */;
        virtual void AddClass(m3d::Class* rtClass) /* 0x04 */;
        virtual void RemoveClass(m3d::Class* rtClass) /* 0x08 */;
        virtual m3d::Class* FindClass(const char* className) /* 0x0c */;
        virtual void GetListOfClasses(m3d::Class**& classList, unsigned int& numOfClasses) /* 0x10 */;
        virtual m3d::Object* New(const char* className) /* 0x18 */;
        virtual m3d::Object* New(m3d::Class* cl) /* 0x18 */;
        virtual m3d::Object* RegisterGlobal(m3d::Object* object, const char* name) /* 0x1c */;
        virtual m3d::Object* FindGlobal(const char* name) /* 0x20 */;
        virtual void UnRegisterGlobal(const char* name) /* 0x24 */;
        virtual void UnRegisterGlobalObject(const m3d::Object* object) /* 0x28 */;
        virtual void DumpMem(const char* fileName) /* 0x2c */;
        virtual void TurnAggressiveMemoryDebugMode(bool bOn) /* 0x30 */;
        virtual unsigned int debugMemUsed() const /* 0x34 */;
        virtual unsigned int debugMemAllocated() const /* 0x38 */;
        virtual unsigned int debugMemOverhead() const /* 0x3c */;
        virtual int debugMemLastAllocSize() const /* 0x40 */;
        virtual void SysError(const CStr& whence, const CStr& descr) /* 0x44 */;
        virtual m3d::cmn::IniFile* CreateIniFile() /* 0x48 */;
        virtual m3d::cmn::XmlFile* CreateXmlFile() /* 0x4c */;
        virtual m3d::cmn::Timer& GetTimer() /* 0x50 */;
        virtual m3d::fs::FileServer& GetFileServer() /* 0x54 */;
        virtual m3d::ScriptServer& GetScriptServer() /* 0x58 */;
        virtual m3d::EngineConfig& GetEngineCfg() /* 0x5c */;
        virtual CStr GetClipboardData() const /* 0x60 */;
        virtual void SetClipboardData(const char* str) const /* 0x64 */;
        bool OpenLog(const char* logFileName);
        virtual void KernelLog(const char* str, ...) /* 0x68 */;
        virtual int MessageBoxA(HWND__* hWnd, const char* pszText, const char* pszCaption, unsigned int uType) /* 0x6c */;

        BYTE _offset[0xB];
        EngineConfig* m_engineConfig;
        ScriptServer* m_scriptServer;
    };
}