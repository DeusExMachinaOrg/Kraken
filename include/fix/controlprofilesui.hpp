#pragma once

// Native "Profiles" tab for the in-game Options menu. Adds a 5th tab button
// (data: optionswnd.xml id 10006) and, via a detour on OptionsWnd::OnWndNotify,
// shows a Kraken-driven page (ProfileOptionsWnd.xml) listing control profiles and
// connected controllers, with create / delete / activate. The page is built from
// its prefab with m3d::ui::LoadDialog and driven entirely from here. See
// Kraken/docs/control-profiles.md (Layer 2).
namespace kraken::fix::controlprofilesui {
    void Apply();
}
