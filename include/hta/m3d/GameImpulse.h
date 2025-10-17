#pragma once
#include "utils.hpp"

#include "Object.h"

class AuxImpulseInfo;
class Event;

namespace ui {
    class Wnd;
}

namespace m3d
{
    struct IImpulse
    {
        virtual ~IImpulse() = default;
        virtual int BindKey1(CStr const&, CStr const&, CStr const&) = 0;
        virtual int BindKey2(CStr const&, CStr const&, CStr const&, CStr const&) = 0;
        virtual int BindKey3(CStr const&, CStr const&, CStr const&, CStr const&, CStr const&) = 0;
        virtual int UnbindKey1(CStr const&, CStr const&, CStr const&) = 0;
        virtual int UnbindKey2(CStr const&, CStr const&, CStr const&, CStr const&) = 0;
        virtual int UnbindKey3(CStr const&, CStr const&, CStr const&, CStr const&, CStr const&) = 0;
        virtual void UnbindAll() = 0;
        virtual int Init() = 0;
        virtual int Done() = 0;
        virtual int LoadFromDefaults() = 0;
        virtual int LoadFromProfile() = 0;
        virtual int SaveToDefaults() = 0;
        virtual int SaveToProfile() = 0;
        virtual int SetImpulseState(const AuxImpulseInfo*, ui::Wnd*) = 0;
        virtual bool GetImpulseState(int) = 0;
        virtual bool GetImpulseStateAndReset(int) = 0;
        virtual void ResetImpulseWithoutNotification(int) = 0;
        virtual void ResetAllImpulses(bool) = 0;
        virtual void RaiseOneTimeImpulse(const AuxImpulseInfo*) = 0;
        virtual int HandleKeyboardMouseEvent(const Event*, ui::Wnd*) = 0;
        virtual int GetKeyIdByName(CStr const&) = 0;
        virtual CStr GetKeyNameById(int) = 0;
        virtual int GetImpulseIdByName(CStr const&) = 0;
        virtual CStr GetImpulseNameById(int) = 0;
        virtual int GetGameModeIdByName(CStr const&) = 0;
        virtual CStr GetGameModeNameById(int) = 0;
        virtual stable_size_vector<stable_size_vector<int>> GetKeysForImpulse(int, int) = 0;
        virtual int GetImpulseForKeys(stable_size_vector<int>, int) = 0;
    };

    struct KeysSet
    {
        stable_size_set<int> Set;

        bool IsThere(int k)
        {
            FUNC(0x005969B0, bool, __thiscall, _IsThere, KeysSet*, int);
            return _IsThere(this, k);
        }
    };

    struct KeyBindStation
    {
        struct BindKey
        {
            stable_size_vector<m3d::KeysSet> m_keys;
        };

        stable_size_vector<m3d::KeyBindStation::BindKey> m_bindings;
    };

    struct GameImpulse : IImpulse, m3d::Object
    {
        virtual void Dtor() = 0;

        int m_refCount;
        int m_parent;
        bool m_isInited;
        bool m_isBinded;
        bool m_bSuppressEvent;
        stable_size_map<int, m3d::KeyBindStation> m_bindings;
        stable_size_map<int, bool> m_impulseStates;
        stable_size_map<int, bool> m_impulseResetAfterRead;
        m3d::KeysSet CurKeys;
        CStr m_profileFileName;

        bool GetImpulseState(int impId)
        {
            FUNC(0x00597250, bool, __thiscall, _GetImpulseState, GameImpulse*, int);
            return _GetImpulseState(this, impId);
        }
        
        bool GetImpulseStateAndReset(int impId)
        {
            FUNC(0x005946E0, bool, __thiscall, _GetImpulseStateAndReset, GameImpulse*, int);
            return _GetImpulseStateAndReset(this, impId);
        }
    };
}