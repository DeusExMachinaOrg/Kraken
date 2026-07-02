#define LOGGER "inputprofiles"

#include <windows.h>
#include <stdint.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

#include "fix/inputprofiles.hpp"
#include "fix/controls.hpp"
#include "fix/gamepad.hpp"
#include "fix/dualsense.hpp"
#include "fix/xinputrumble.hpp"
#include "config.hpp"
#include "ext/logger.hpp"
#include "ext/impulse.hpp"

#include "hta/CMiracle3d.hpp"
#include "hta/CStr.hpp"
#include "hta/m3d/GameImpulse.hpp"

// All addresses are VAs for hta.exe (image base 0x400000).
namespace kraken::fix::inputprofiles {
    namespace {
        // m3d::Application* g_pApp == CMiracle3d::Instance().
        constexpr uintptr_t G_PAPP_VA      = 0x00A0A55C;
        // ProfileManager* lives at g_pApp + this (see TruxxImpulse::GetProfileFolder
        // @0x1508c0, which reads [g_pApp+0x8b52c] then +0x44 for the current name).
        constexpr uintptr_t PROFILEMGR_OFF = 0x8b52c;
        // CStr of the active player profile's name inside ProfileManager.
        constexpr uintptr_t PROFILE_NAME_OFF = 0x44;

        const char* const KRAKEN_INI = "./data/kraken.ini";

        bool   g_activated = false; // first activation (migration) done
        std::string g_lastPlayer;   // player folder we last activated for

        // --- engine: active player profile -------------------------------------

        std::string PlayerName() {
            void* app = *reinterpret_cast<void**>(G_PAPP_VA);
            if (!app)
                return "";
            char* pm = *reinterpret_cast<char**>(static_cast<char*>(app) + PROFILEMGR_OFF);
            if (!pm)
                return "";
            // CStr at pm+0x44; its data pointer is the first field.
            const char* name = *reinterpret_cast<const char* const*>(pm + PROFILE_NAME_OFF);
            if (!name || !name[0])
                return "";
            return name;
        }

        std::string PlayerDir() {
            std::string name = PlayerName();
            if (name.empty())
                return "";
            return std::string("./data/profiles/") + name + "/";
        }

        // --- tiny Win32 filesystem helpers -------------------------------------

        bool DirExists(const std::string& p) {
            DWORD a = GetFileAttributesA(p.c_str());
            return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
        }
        bool FileExists(const std::string& p) {
            DWORD a = GetFileAttributesA(p.c_str());
            return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
        }

        // Create every missing segment of a path (segments separated by '/').
        void MakeDirs(const std::string& path) {
            std::string acc;
            for (size_t i = 0; i < path.size(); ++i) {
                char c = path[i];
                acc += c;
                if (c == '/' || c == '\\') {
                    if (acc.size() > 1 && acc != "./" && acc != ".\\")
                        CreateDirectoryA(acc.c_str(), nullptr);
                }
            }
            if (!acc.empty() && acc.back() != '/' && acc.back() != '\\')
                CreateDirectoryA(acc.c_str(), nullptr);
        }

        std::vector<std::string> SubDirs(const std::string& root) {
            std::vector<std::string> out;
            WIN32_FIND_DATAA fd = {};
            HANDLE h = FindFirstFileA((root + "*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE)
                return out;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    continue;
                if (std::strcmp(fd.cFileName, ".") == 0 || std::strcmp(fd.cFileName, "..") == 0)
                    continue;
                out.emplace_back(fd.cFileName);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
            return out;
        }

        // Recursively delete a directory tree (profiles are small: input.ini,
        // meta.ini, optional keybindings.lua).
        void DeleteTree(const std::string& dir) {
            WIN32_FIND_DATAA fd = {};
            HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (std::strcmp(fd.cFileName, ".") == 0 || std::strcmp(fd.cFileName, "..") == 0)
                        continue;
                    std::string child = dir + fd.cFileName;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        DeleteTree(child + "/");
                    else
                        DeleteFileA(child.c_str());
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
            RemoveDirectoryA(dir.c_str());
        }

        bool ValidName(const std::string& name) {
            if (name.empty() || name.size() > 48)
                return false;
            // Keep it a safe folder name (no path separators / reserved chars).
            return name.find_first_of("\\/:*?\"<>|") == std::string::npos
                && name != "." && name != "..";
        }

        std::string ProfileDir(const std::string& name) {
            return Root() + name + "/";
        }
        std::string ProfileIni(const std::string& name) {
            return ProfileDir(name) + "input.ini";
        }
        std::string ProfileKeybindings(const std::string& name) {
            return ProfileDir(name) + "keybindings.lua";
        }

        hta::m3d::GameImpulse* Impulses() {
            return reinterpret_cast<hta::m3d::GameImpulse*>(
                hta::CMiracle3d::Instance()->m_pImpulses);
        }

        void WriteMeta(const std::string& name, const std::string& deviceKind) {
            std::string meta = ProfileDir(name) + "meta.ini";
            WritePrivateProfileStringA("meta", "name", name.c_str(), meta.c_str());
            WritePrivateProfileStringA("meta", "device_kind", deviceKind.c_str(), meta.c_str());
        }

        std::string ReadMeta(const std::string& name, const char* key, const char* def) {
            char buf[256] = {0};
            std::string meta = ProfileDir(name) + "meta.ini";
            GetPrivateProfileStringA("meta", key, def, buf, sizeof(buf), meta.c_str());
            return buf;
        }

        void PersistActive(const std::string& name) {
            Config::Instance().active_input_profile.value = name;
            WritePrivateProfileStringA("input", "active_profile", name.c_str(), KRAKEN_INI);
        }

        // Resolve the profile to activate: the configured one if it still exists,
        // else the first available.
        std::string ResolveActive(const std::vector<std::string>& names) {
            std::string active = Config::Instance().active_input_profile.value;
            if (!active.empty() &&
                std::find(names.begin(), names.end(), active) != names.end())
                return active;
            return names.empty() ? "" : names.front();
        }

        // First-time setup for a player: migrate the current kraken.ini input
        // sections into a "default" profile if none exist, then activate.
        void EnsureActivated() {
            if (!Available())
                return;
            std::string player = PlayerDir();
            if (g_activated && player == g_lastPlayer)
                return; // already set up for this player

            MakeDirs(Root());
            std::vector<std::string> names = SubDirs(Root());
            if (names.empty()) {
                // Migrate: the in-memory input config currently mirrors kraken.ini
                // (input source not yet redirected), so Create snapshots it.
                if (Create("default", "custom")) {
                    LOG_INFO("Migrated kraken.ini input sections -> profile 'default'");
                    names = SubDirs(Root());
                }
            }

            std::string active = ResolveActive(names);
            if (active.empty())
                return;
            if (Switch(active)) {
                g_activated  = true;
                g_lastPlayer = player;
            }
        }

        // --- engine bindings-load detour ---------------------------------------
        // The engine (re)loads its key bindings via GameImpulse::LoadFromDefaults /
        // LoadFromProfile (each runs UnbindAll first). That is the moment the player
        // profile is known and the moment any JOY_BUTTON binds get wiped, so we hook
        // both: after the original runs we activate / re-apply the control profile
        // (which also re-binds the gamepad buttons via gamepad::Reapply).
        using LoadFn = int(__thiscall*)(void* self);
        LoadFn g_origLoadDefaults = nullptr;
        LoadFn g_origLoadProfile  = nullptr;

        void AfterBindingsLoaded() {
            // Activates on first call (with migration); on later reloads it re-runs
            // Switch to re-apply device config + re-bind the gamepad after UnbindAll.
            if (!g_activated) {
                EnsureActivated();
                return;
            }
            std::string player = PlayerDir();
            if (player != g_lastPlayer) { // player switched profiles mid-session
                g_activated = false;
                EnsureActivated();
                return;
            }
            std::string active = Config::Instance().active_input_profile.value;
            if (!active.empty())
                Switch(active);
        }

        int __fastcall LoadDefaults_Hook(void* self, void*) {
            int r = g_origLoadDefaults(self);
            AfterBindingsLoaded();
            return r;
        }
        int __fastcall LoadProfile_Hook(void* self, void*) {
            int r = g_origLoadProfile(self);
            AfterBindingsLoaded();
            return r;
        }

        // Entry detour with a runtime trampoline (relocated prologue + jmp back).
        LoadFn InstallLoadDetour(uintptr_t addr, int prologueLen, void* hook) {
            uint8_t* fn = reinterpret_cast<uint8_t*>(addr);
            uint8_t* tramp = static_cast<uint8_t*>(
                VirtualAlloc(nullptr, prologueLen + 5, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE));
            if (!tramp) {
                LOG_ERROR("Failed to allocate load-detour trampoline @ 0x%p", (void*)addr);
                return nullptr;
            }
            std::memcpy(tramp, fn, prologueLen);
            tramp[prologueLen] = 0xE9;
            *reinterpret_cast<int32_t*>(tramp + prologueLen + 1) =
                static_cast<int32_t>((fn + prologueLen) - (tramp + prologueLen + 5));

            DWORD prot;
            VirtualProtect(fn, prologueLen, PAGE_EXECUTE_READWRITE, &prot);
            fn[0] = 0xE9;
            *reinterpret_cast<int32_t*>(fn + 1) =
                static_cast<int32_t>(reinterpret_cast<uint8_t*>(hook) - (fn + 5));
            for (int i = 5; i < prologueLen; ++i)
                fn[i] = 0x90;
            VirtualProtect(fn, prologueLen, prot, &prot);
            return reinterpret_cast<LoadFn>(tramp);
        }
    }

    // --- public API -----------------------------------------------------------

    bool Available() {
        return !PlayerDir().empty();
    }

    std::string Root() {
        std::string dir = PlayerDir();
        if (dir.empty())
            return "";
        return dir + "input_profiles/";
    }

    std::string Active() {
        return Config::Instance().active_input_profile.value;
    }

    std::vector<ProfileMeta> List() {
        std::vector<ProfileMeta> out;
        std::string root = Root();
        if (root.empty())
            return out;
        for (const std::string& name : SubDirs(root)) {
            ProfileMeta m;
            m.name        = name;
            m.display     = ReadMeta(name, "name", name.c_str());
            m.device_kind = ReadMeta(name, "device_kind", "custom");
            out.push_back(std::move(m));
        }
        return out;
    }

    bool Create(const std::string& name, const std::string& deviceKind) {
        if (!Available() || !ValidName(name))
            return false;
        std::string dir = ProfileDir(name);
        if (DirExists(dir))
            return false; // already exists
        MakeDirs(dir);

        // Snapshot the current in-memory input config into the new profile's
        // input.ini by pointing Config's input source at it for the dump, then
        // restoring whatever source was active.
        Config& c = Config::Instance();
        std::string prev = (Active().empty() || !FileExists(ProfileIni(Active())))
                               ? std::string()
                               : ProfileIni(Active());
        c.SetInputSource(ProfileIni(name));
        c.DumpInput();
        c.SetInputSource(prev); // restore (empty => kraken.ini)
        WriteMeta(name, deviceKind);

        // Baseline keybindings: snapshot whatever is live right now (the game's
        // current keyboard/mouse + JOY_BUTTON_* bindings), so the new profile
        // starts from a sane, working set instead of nothing.
        SyncBindingsToProfile(name);

        LOG_INFO("Created control profile '%s' (%s)", name.c_str(), deviceKind.c_str());
        return true;
    }

    bool Rename(const std::string& from, const std::string& to) {
        if (!Available() || !ValidName(from) || !ValidName(to))
            return false;
        if (!DirExists(ProfileDir(from)) || DirExists(ProfileDir(to)))
            return false;
        std::string a = ProfileDir(from);
        std::string b = ProfileDir(to);
        if (!a.empty() && (a.back() == '/' || a.back() == '\\')) a.pop_back();
        if (!b.empty() && (b.back() == '/' || b.back() == '\\')) b.pop_back();
        if (!MoveFileA(a.c_str(), b.c_str()))
            return false;
        WriteMeta(to, ReadMeta(to, "device_kind", "custom"));
        if (Active() == from)
            PersistActive(to);
        return true;
    }

    bool Delete(const std::string& name) {
        if (!Available() || !ValidName(name) || !DirExists(ProfileDir(name)))
            return false;
        std::vector<std::string> names = SubDirs(Root());
        if (names.size() <= 1)
            return false; // keep at least one profile
        DeleteTree(ProfileDir(name));
        if (Active() == name) {
            names = SubDirs(Root());
            std::string next = names.empty() ? "" : names.front();
            if (!next.empty())
                Switch(next);
        }
        return true;
    }

    bool Switch(const std::string& name) {
        if (!Available())
            return false;
        std::string ini = ProfileIni(name);
        if (!FileExists(ini))
            return false;

        bool changed = (Active() != name);
        if (changed)
            PersistActive(name);

        Config& c = Config::Instance();
        c.SetInputSource(ini);
        c.ReloadInput();

        // Load this profile's own key/button bindings (keyboard/mouse + any
        // JOY_BUTTON_* saved into it) BEFORE the input modules re-apply, so
        // gamepad::Reapply's ini-driven autobind (if enabled) gets the final say
        // over JOY_BUTTON_* — matching its existing "ini is authority" semantics.
        // GameImpulse::LoadFromFile executes a BindKeyN(...) script (like
        // defaultkeybindings.lua) but does NOT call UnbindAll itself, so we do it
        // first (mirrors the engine's own LoadFromDefaults/LoadFromProfile).
        std::string keys = ProfileKeybindings(name);
        hta::m3d::GameImpulse* impulses = Impulses();
        if (impulses && impulses->m_isInited && FileExists(keys)) {
            impulses->UnbindAll();
            hta::CStr path = keys.c_str();
            impulses->LoadFromFile(path);
        }

        // Re-apply every input module from the freshly-loaded config.
        controls::Reapply();
        gamepad::Reapply();      // re-bind JOY_BUTTON_* (ini authority, if enabled)
        dualsense::Reapply();
        xinputrumble::Reapply();

        if (changed)
            LOG_INFO("Switched control profile -> '%s'", name.c_str());
        return true;
    }

    void SyncBindingsToProfile(const std::string& name) {
        if (!Available() || !DirExists(ProfileDir(name)))
            return;
        hta::m3d::GameImpulse* impulses = Impulses();
        if (!impulses || !impulses->m_isInited)
            return;
        hta::CStr path = ProfileKeybindings(name).c_str();
        impulses->SaveToFile(path);
    }

    std::vector<impulse::DeviceInfo> Devices() {
        return impulse::GetDevices();
    }

    bool SetDevice(const std::string& profile, uint32_t deviceId) {
        if (!Available() || !FileExists(ProfileIni(profile)))
            return false;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%u", deviceId);
        WritePrivateProfileStringA("wheel", "device", buf, ProfileIni(profile).c_str());
        WriteMeta(profile, ReadMeta(profile, "device_kind", "custom"));
        if (Active() == profile)
            Switch(profile); // reload + re-apply the input modules with the new device
        LOG_INFO("Profile '%s' device set to winmm %u", profile.c_str(), deviceId);
        return true;
    }

    void Apply() {
        // LoadFromProfile prologue is 5 bytes (sub esp,0x48 / push ebx / push esi);
        // LoadFromDefaults is 6 (sub esp,0x18 / push esi / mov esi,ecx).
        g_origLoadDefaults = InstallLoadDetour(0x00597990, 6, (void*)&LoadDefaults_Hook);
        g_origLoadProfile  = InstallLoadDetour(0x00597AB0, 5, (void*)&LoadProfile_Hook);
        LOG_INFO("Control profiles enabled (activate on engine bindings load)");
    }
}
