#include "codex/TextureDX9.hpp"

#include "hta/CStr.hpp"
#include "hta/m3d/Application.hpp"
#include "hta/m3d/rend/IRenderer.hpp"

namespace kraken::codex {
    static const String sExts[] = { ".dds", ".tga", ".bmp" };

    std::span<const String> TextureDX9::Exts() const {
        return { sExts };
    }

    assets::Asset* TextureDX9::Load(const String& path) {
        auto* renderer = hta::m3d::Application::Instance()->m_renderer;
        if (!renderer)
            return nullptr;

        hta::CStr name(static_cast<const char*>(path));
        auto handle = renderer->AddTexture(name, 0);
        if (handle == -1)
            return nullptr;

        auto* tex = new assets::Texture();
        tex->mHandle = handle;
        tex->mLabel = path;
        return tex;
    }

    void TextureDX9::Save(const String& path, const assets::Asset& asset) {
        // Not supported
    }
}
