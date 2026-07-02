#ifndef KRAKEN_FIX_INPUTPROFILES
#define KRAKEN_FIX_INPUTPROFILES

#include <string>
#include <vector>

#include "ext/impulse.hpp"

// Control profiles: named bundles of the input device config ([wheel]/[gamepad]/
// [dualsense]/[xinput]) — one per controller (wheel, DualSense, XInput, ...) —
// stored inside the active player profile at
//   data/profiles/<player>/input_profiles/<name>/input.ini
// The active profile's input.ini feeds Config's input sections (see
// Config::SetInputSource / ReloadInput); switching re-reads it and re-applies the
// input modules live (controls / gamepad / dualsense / xinputrumble). See
// Kraken/docs/control-profiles.md.
namespace kraken::fix::inputprofiles {
    struct ProfileMeta {
        std::string name;        // folder name (== id)
        std::string display;     // display name (meta.ini, defaults to folder)
        std::string device_kind; // "wheel" / "dualsense" / "xinput" / "custom"
    };

    // Install the engine-bindings-load detour so profiles activate at game start
    // and survive keybinding reloads. Cheap; the actual work defers until the
    // player profile is known.
    void Apply();

    // True once the active player profile folder is resolvable.
    bool Available();

    // Profiles directory for the active player ("" if not available yet).
    std::string Root();

    // Enumerate the control profiles for the active player.
    std::vector<ProfileMeta> List();

    // Active profile folder name (from kraken.ini [input] active_profile).
    std::string Active();

    // Create a new profile from the current in-memory input config (clone of the
    // active settings). Returns false on bad name / fs error.
    bool Create(const std::string& name, const std::string& deviceKind = "custom");

    bool Rename(const std::string& from, const std::string& to);
    bool Delete(const std::string& name);

    // Make <name> the active profile: re-read its input.ini into Config, re-apply
    // every input module without a restart, and load the profile's own key/button
    // bindings (keybindings.lua) into the engine (UnbindAll + LoadFromFile). Returns
    // false if the profile is missing.
    bool Switch(const std::string& name);

    // Snapshot the engine's LIVE key/button bindings (GameImpulse — this covers
    // both keyboard/mouse and JOY_BUTTON_* since the engine keeps them in the same
    // bind stations) into the given profile's keybindings.lua. Called after Create
    // (baseline) and whenever the stock BindKeysWnd "Apply" button commits an edit
    // (see fix/controlprofilesui.cpp), so the profile always reflects what's live.
    void SyncBindingsToProfile(const std::string& name);

    // Connected controllers (winmm view), for the device picker in the menu.
    std::vector<impulse::DeviceInfo> Devices();

    // Assign a controller (winmm device id) to a profile's [wheel] device. If the
    // profile is the active one, the change is reloaded and re-applied live.
    bool SetDevice(const std::string& profile, uint32_t deviceId);
}

#endif
