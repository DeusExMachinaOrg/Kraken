#define LOGGER "inputprofiles"

#include <windows.h>
#include <stdint.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
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
        bool   g_suppressReapply = false; // guard around the stock Default button

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
        // True if a keybindings.lua actually contains bindings (not just the
        // UnbindAll header). Used to decide whether it is worth cloning as a
        // baseline for a new profile.
        bool HasRealBindings(const std::string& p) {
            FILE* f = std::fopen(p.c_str(), "rb");
            if (!f) return false;
            char buf[4096];
            size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
            std::fclose(f);
            buf[n] = '\0';
            return std::strstr(buf, "BindKey") != nullptr;
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

        // One-time migration of the old per-player control profiles
        // (./data/profiles/<player>/input_profiles/*) into the new global store.
        // Copies each profile's small file set; skips any that already exist
        // globally. Returns true if anything was brought over.
        bool MigrateLegacyPlayerProfiles() {
            std::string legacy = PlayerDir();
            if (legacy.empty())
                return false;
            legacy += "input_profiles/";
            if (!DirExists(legacy))
                return false;
            bool any = false;
            for (const std::string& s : SubDirs(legacy)) {
                std::string src = legacy + s + "/";
                std::string dst = ProfileDir(s); // global
                if (DirExists(dst))
                    continue;
                MakeDirs(dst);
                static const char* kFiles[] = { "input.ini", "meta.ini", "keybindings.lua" };
                for (const char* f : kFiles)
                    CopyFileA((src + f).c_str(), (dst + f).c_str(), FALSE);
                any = true;
            }
            if (any)
                LOG_INFO("Migrated legacy per-player control profiles -> global store");
            return any;
        }

        // First-time setup: populate the global control-profile store (migrate old
        // per-player profiles, else snapshot kraken.ini into a "default"), then
        // activate. Re-runs when the player changes so the active profile is
        // re-applied over the engine's freshly-loaded bindings.
        void EnsureActivated() {
            if (!Available())
                return;
            std::string player = PlayerDir();
            if (g_activated && player == g_lastPlayer)
                return; // already set up for this player

            MakeDirs(Root());
            std::vector<std::string> names = SubDirs(Root());
            if (names.empty() && MigrateLegacyPlayerProfiles())
                names = SubDirs(Root());
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
            // Skip the re-apply when the stock Default button is loading the
            // game's default bindings (controlprofilesui sets this): reloading the
            // profile's keybindings.lua here would clobber the freshly-loaded
            // defaults (and wipe everything if that file happens to be empty).
            if (g_suppressReapply)
                return;
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
        // Control profiles are now GLOBAL (not tied to a player profile), so they
        // are usable as soon as the game application object exists.
        return *reinterpret_cast<void**>(G_PAPP_VA) != nullptr;
    }

    // Global control-profile storage, shared across all player profiles.
    std::string Root() {
        return "./data/input_profiles/";
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

        // Baseline keybindings. Prefer CLONING the active profile's saved
        // keybindings.lua: while the bindings editor (BindKeysWnd) is open — which
        // is exactly when the "New" button lives — the live GameImpulse set is
        // empty, so a live snapshot here would write an empty file. The active
        // profile's on-disk file still holds the real bindings, so clone it when
        // it has any; otherwise fall back to a live snapshot.
        std::string srcKeys = Active().empty() ? std::string() : ProfileKeybindings(Active());
        if (!srcKeys.empty() && FileExists(srcKeys) && HasRealBindings(srcKeys))
            CopyFileA(srcKeys.c_str(), ProfileKeybindings(name).c_str(), FALSE);
        else
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
        // Load this profile's bindings, but only when the file actually HAS
        // bindings — an empty/near-empty keybindings.lua must NOT clear the live
        // set (otherwise activating it, incl. at startup, wipes every hotkey).
        std::string keys = ProfileKeybindings(name);
        hta::m3d::GameImpulse* impulses = Impulses();
        if (impulses && impulses->m_isInited && FileExists(keys) && HasRealBindings(keys)) {
            // The engine's script server (ScriptServer::executeScriptFile) resolves
            // paths relative to the game root and CANNOT open a "./"-prefixed path
            // (its own files load as "data\..."); strip a leading "./" or ".\".
            // Also do NOT UnbindAll ourselves: keybindings.lua begins with
            // IMPULSES:UnbindAll(), so on success it clears+rebinds atomically, and
            // on a failed load the live bindings are left intact instead of wiped.
            std::string sp = keys;
            if (sp.size() >= 2 && sp[0] == '.' && (sp[1] == '/' || sp[1] == '\\'))
                sp.erase(0, 2);
            hta::CStr path = sp.c_str();
            int lr = impulses->LoadFromFile(path);
            if (!lr)
                LOG_WARNING("Failed to load bindings for profile '%s' from '%s'",
                            name.c_str(), sp.c_str());
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
        // Save to a temp file first and only commit it over the profile's real
        // keybindings.lua when it actually contains bindings. This never clobbers
        // a good profile with an empty snapshot — which happens if the live set was
        // wiped (e.g. mid-switch, or before bindings finished loading).
        std::string dst = ProfileKeybindings(name);
        std::string tmp = dst + ".tmp";
        hta::CStr path = tmp.c_str();
        impulses->SaveToFile(path);
        if (HasRealBindings(tmp)) {
            MoveFileExA(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);
        } else {
            DeleteFileA(tmp.c_str());
            LOG_WARNING("Not saving empty bindings snapshot to profile '%s'", name.c_str());
        }
    }

    void SuppressReapply(bool on) {
        g_suppressReapply = on;
    }

    // Synthetic device id for the native DualSense (HID). Kept above the winmm
    // range (0..15) so it never collides. dualsense.cpp injects its connection /
    // axes under [wheel] device, so selecting this id makes the two agree.
    static const uint32_t kNativeDualSenseDeviceId = 100;

    std::string DeviceNameFor(const std::string& profile) {
        char buf[128] = {0};
        GetPrivateProfileStringA("wheel", "device_name", "", buf, sizeof(buf),
                                 ProfileIni(profile).c_str());
        return buf;
    }

    std::vector<impulse::DeviceInfo> Devices() {
        std::vector<impulse::DeviceInfo> out = impulse::GetDevices();
        LOG_INFO("Devices(): GetDevices()=%zu NativePresent=%d NativeName='%s' AnyXInput=%d",
                 out.size(), (int)dualsense::NativePresent(),
                 dualsense::NativePresent() ? dualsense::NativeName() : "",
                 (int)xinputrumble::AnyConnected());
        for (const auto& d : out)
            LOG_INFO("  winmm dev id=%u btn=%u ax=%u name='%s'",
                     d.id, d.buttons, d.axes, d.name);
        // Offer the native DualSense (opened over HID) as its own named entry — it
        // is not a winmm joystick, so it is absent from GetDevices(). But NOT when
        // an XInput pad is present (DS4Windows / DSX / Steam Input): those bridges
        // present a virtual Xbox pad yet leave the HID shareably open, so "HID
        // open" alone would wrongly read as native mode.
        if (dualsense::NativePresent()) {
            const char* nativeName = dualsense::NativeName();
            // Whenever the DualSense HID is open, drop its winmm view so it isn't
            // listed twice: by name when the OEM lookup resolved it, else by the
            // DualSense's winmm signature (14 buttons / 6 axes) — its generic
            // "Microsoft PC-joystick driver" name matches nothing. (In native mode
            // this duplicates our HID entry; with a DSX/Steam bridge the winmm view
            // is redundant since input flows through the virtual pad.)
            out.erase(std::remove_if(out.begin(), out.end(),
                [&](const impulse::DeviceInfo& d) {
                    return _stricmp(d.name, nativeName) == 0
                        || std::strstr(d.name, "DualSense") != nullptr
                        || std::strstr(d.name, "Wireless Controller") != nullptr
                        || (d.buttons == 14 && d.axes == 6);
                }), out.end());
            // Offer the native HID entry only in true native mode (no XInput bridge
            // present); with a bridge, input goes through the virtual pad instead.
            if (!xinputrumble::AnyConnected()) {
                impulse::DeviceInfo ds = {};
                ds.id = kNativeDualSenseDeviceId;
                strncpy_s(ds.name, nativeName, _TRUNCATE);
                out.push_back(ds);
            }
        }
        LOG_INFO("Devices(): final=%zu", out.size());
        for (const auto& d : out)
            LOG_INFO("  -> id=%u btn=%u ax=%u name='%s'",
                     d.id, d.buttons, d.axes, d.name);
        return out;
    }

    void ApplyDeviceDefaults(const std::string& profile) {
        if (!Available() || !FileExists(ProfileIni(profile)))
            return;
        std::string ini = ProfileIni(profile);
        auto setInt = [&](const char* section, const char* key, int val) {
            char b[16];
            std::snprintf(b, sizeof(b), "%d", val);
            WritePrivateProfileStringA(section, key, b, ini.c_str());
        };
        // Treat it as the native DualSense only when its HID is open AND no XInput
        // pad is present. DS4Windows / DSX / Steam Input expose a virtual Xbox
        // (XInput) pad while leaving the DualSense HID shareably open, so testing
        // NativePresent() alone misfires as native — prefer XInput in that case.
        if (dualsense::NativePresent() && !xinputrumble::AnyConnected()) {
            // Native DualSense HID axis layout: 0=LX 1=LY 2=RX 3=RY 4=L2 5=R2.
            setInt("dualsense", "enabled", 1);
            setInt("wheel", "device", static_cast<int>(kNativeDualSenseDeviceId));
            setInt("wheel", "steer_axis", 0);
            setInt("wheel", "throttle_axis", 5);
            setInt("wheel", "brake_axis", 4);
            setInt("wheel", "trigger_axis", -1);
            setInt("wheel", "cam_yaw_axis", 2);
            setInt("wheel", "cam_pitch_axis", 3);
            setInt("wheel", "invert_steer", 1);
            setInt("wheel", "invert_throttle", 0);
            setInt("wheel", "invert_brake", 0);
            setInt("wheel", "invert_trigger", 0);
            LOG_INFO("Applied native DualSense axis layout to profile '%s'", profile.c_str());
        } else {
            // XInput / winmm gamepad layout: 0=LX 1=LY 2=triggers(combined) 3=RY
            // 4=RX. Keep whatever winmm device is already selected.
            setInt("wheel", "steer_axis", 0);
            setInt("wheel", "trigger_axis", 2);
            setInt("wheel", "throttle_axis", 5);
            setInt("wheel", "brake_axis", 4);
            setInt("wheel", "cam_yaw_axis", 4);
            setInt("wheel", "cam_pitch_axis", 3);
            setInt("wheel", "invert_steer", 1);
            setInt("wheel", "invert_trigger", 1);
            LOG_INFO("Applied XInput gamepad axis layout to profile '%s'", profile.c_str());
        }
        if (Active() == profile) {
            Config::Instance().SetInputSource(ini);
            Config::Instance().ReloadInput();
            controls::Reapply();
            dualsense::Reapply();
        }
    }

    bool SetDevice(const std::string& profile, uint32_t deviceId) {
        if (!Available() || !FileExists(ProfileIni(profile)))
            return false;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%u", deviceId);
        WritePrivateProfileStringA("wheel", "device", buf, ProfileIni(profile).c_str());
        // Remember the human-readable device name so the picker can still show it
        // (prefixed [X]) when the controller is later disconnected.
        for (const impulse::DeviceInfo& d : Devices()) {
            if (d.id == deviceId) {
                WritePrivateProfileStringA("wheel", "device_name", d.name, ProfileIni(profile).c_str());
                break;
            }
        }
        WriteMeta(profile, ReadMeta(profile, "device_kind", "custom"));
        if (Active() == profile)
            Switch(profile); // reload + re-apply the input modules with the new device
        LOG_INFO("Profile '%s' device set to winmm %u", profile.c_str(), deviceId);
        return true;
    }

    // Re-read the active profile's input.ini into Config and re-apply the live
    // input modules (no restart). Shared by SetAxis / SetInvert so a mapping edit
    // takes effect in the sim immediately, exactly like ApplyDeviceDefaults' tail.
    static void ReapplyActive(const std::string& profile) {
        if (Active() != profile)
            return;
        std::string ini = ProfileIni(profile);
        Config::Instance().SetInputSource(ini);
        Config::Instance().ReloadInput();
        controls::Reapply();
        dualsense::Reapply();
        xinputrumble::Reapply();
    }

    bool SetAxis(const std::string& profile, const char* iniKey, int axisIndex) {
        if (!Available() || !FileExists(ProfileIni(profile)))
            return false;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", axisIndex);
        WritePrivateProfileStringA("wheel", iniKey, buf, ProfileIni(profile).c_str());
        ReapplyActive(profile);
        LOG_INFO("Profile '%s' [wheel] %s = %d", profile.c_str(), iniKey, axisIndex);
        return true;
    }

    bool SetInvert(const std::string& profile, const char* iniKey, bool on) {
        if (!Available() || !FileExists(ProfileIni(profile)))
            return false;
        WritePrivateProfileStringA("wheel", iniKey, on ? "1" : "0",
                                   ProfileIni(profile).c_str());
        ReapplyActive(profile);
        LOG_INFO("Profile '%s' [wheel] %s = %d", profile.c_str(), iniKey, (int)on);
        return true;
    }

    int GetAxis(const std::string& profile, const char* iniKey, int def) {
        if (!Available())
            return def;
        return GetPrivateProfileIntA("wheel", iniKey, def, ProfileIni(profile).c_str());
    }

    bool GetInvert(const std::string& profile, const char* iniKey) {
        if (!Available())
            return false;
        return GetPrivateProfileIntA("wheel", iniKey, 0, ProfileIni(profile).c_str()) != 0;
    }

    // Generic section/key accessors for the feedback (vibration/triggers/FFB)
    // settings, which live in the same profile input.ini across [wheel]/[dualsense]/
    // [xinput]. Writers re-apply the active profile so edits take effect live.
    bool SetFloat(const std::string& profile, const char* section, const char* key, float v) {
        if (!Available() || !FileExists(ProfileIni(profile)))
            return false;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6f", v);
        WritePrivateProfileStringA(section, key, buf, ProfileIni(profile).c_str());
        ReapplyActive(profile);
        LOG_INFO("Profile '%s' [%s] %s = %.3f", profile.c_str(), section, key, v);
        return true;
    }

    bool SetBool(const std::string& profile, const char* section, const char* key, bool on) {
        if (!Available() || !FileExists(ProfileIni(profile)))
            return false;
        WritePrivateProfileStringA(section, key, on ? "true" : "false",
                                   ProfileIni(profile).c_str());
        ReapplyActive(profile);
        LOG_INFO("Profile '%s' [%s] %s = %d", profile.c_str(), section, key, (int)on);
        return true;
    }

    float GetFloat(const std::string& profile, const char* section, const char* key, float def) {
        if (!Available())
            return def;
        char buf[32], defbuf[32];
        std::snprintf(defbuf, sizeof(defbuf), "%.6f", def);
        GetPrivateProfileStringA(section, key, defbuf, buf, sizeof(buf),
                                 ProfileIni(profile).c_str());
        return static_cast<float>(std::atof(buf));
    }

    bool GetBool(const std::string& profile, const char* section, const char* key, bool def) {
        if (!Available())
            return def;
        char buf[16];
        GetPrivateProfileStringA(section, key, def ? "true" : "false", buf, sizeof(buf),
                                 ProfileIni(profile).c_str());
        // Accept "1"/"true"/"yes" (config.cpp writes bools as true/false).
        return buf[0] == '1' || buf[0] == 't' || buf[0] == 'T'
            || buf[0] == 'y' || buf[0] == 'Y';
    }

    void Apply() {
        // LoadFromProfile prologue is 5 bytes (sub esp,0x48 / push ebx / push esi);
        // LoadFromDefaults is 6 (sub esp,0x18 / push esi / mov esi,ecx).
        g_origLoadDefaults = InstallLoadDetour(0x00597990, 6, (void*)&LoadDefaults_Hook);
        g_origLoadProfile  = InstallLoadDetour(0x00597AB0, 5, (void*)&LoadProfile_Hook);
        LOG_INFO("Control profiles enabled (activate on engine bindings load)");
    }
}
