#include "assets/Texture.hpp"

#include "hta/m3d/Application.hpp"
#include "hta/m3d/rend/IRenderer.hpp"

namespace kraken::assets {
    Texture::Texture() = default;

    Texture::~Texture() {
        if (IsValid()) {
            if (auto* renderer = hta::m3d::Application::Instance()->m_renderer)
                renderer->ReleaseTexture(mHandle);
        }
    }
}
