#include "ext/uibooks/uibooks_resources.hpp"

#include <fstream>

#include "ext/uibooks/uibooks_constants.hpp"
#include "hta/CMiracle3d.hpp"
#include "hta/CStr.hpp"
#include "hta/m3d/rend/IRenderer.hpp"

namespace kraken::ext::uibooks::resources {
    namespace {
        uint32_t ReadLe16(const uint8_t* p) {
            return (uint32_t) p[0] | ((uint32_t) p[1] << 8);
        }

        uint32_t ReadLe32(const uint8_t* p) {
            return (uint32_t) p[0] | ((uint32_t) p[1] << 8)
                 | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
        }
    }

    bool ReadImageDimensions(const char* path, float* width, float* height) {
        if (!path || !width || !height)
            return false;
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;
        uint8_t header[32] = {};
        file.read(reinterpret_cast<char*>(header), sizeof(header));
        if (file.gcount() < 24)
            return false;

        uint32_t w = 0;
        uint32_t h = 0;
        if (header[0] == 'D' && header[1] == 'D' && header[2] == 'S' && header[3] == ' ') {
            h = ReadLe32(header + 12);
            w = ReadLe32(header + 16);
        }
        else if (header[0] == 0x89 && header[1] == 'P' && header[2] == 'N' && header[3] == 'G') {
            // PNG stores IHDR dimensions in network byte order.
            w = ((uint32_t) header[16] << 24) | ((uint32_t) header[17] << 16)
              | ((uint32_t) header[18] << 8) | header[19];
            h = ((uint32_t) header[20] << 24) | ((uint32_t) header[21] << 16)
              | ((uint32_t) header[22] << 8) | header[23];
        }
        else {
            // TGA dimensions are stored at byte offsets 12/14. Only accept
            // the image types supported by the resource loader.
            if (header[2] != 2 && header[2] != 3)
                return false;
            w = ReadLe16(header + 12);
            h = ReadLe16(header + 14);
        }
        if (w == 0 || h == 0 || w > constants::ImageDimensionLimit
            || h > constants::ImageDimensionLimit)
            return false;
        *width = (float) w;
        *height = (float) h;
        return true;
    }

    uint32_t LoadTexture(const char* path) {
        if (!path || !*path)
            return 0;
        const hta::CMiracle3d* app = hta::CMiracle3d::Instance();
        hta::m3d::rend::IRenderer* renderer = app ? app->m_renderer : nullptr;
        if (!renderer)
            return 0;
        const hta::CStr name(path);
        const hta::m3d::rend::TexHandle handle = renderer->AddTexture(
            name, hta::m3d::rend::TEXTURE_LOAD_DEFAULT);
        if (handle.m_handle <= 0)
            return 0;
        return static_cast<uint32_t>(handle.m_handle);
    }

    uint32_t LoadTextureByResourceId(const char* id) {
        if (!id || !*id)
            return 0;
        const hta::CMiracle3d* app = hta::CMiracle3d::Instance();
        if (!app || !app->m_pInterfaceManager)
            return 0;

        // The UI manager owns the icon registry populated from uiicons.xml and
        // the referenced miscicons.xml/ui resource files. The returned handle
        // is borrowed from that registry and must not be released by books.
        const hta::m3d::rend::TexHandle handle =
            app->m_pInterfaceManager->GetIcoByName(hta::CStr(id), 0);
        return handle.m_handle > 0 ? static_cast<uint32_t>(handle.m_handle) : 0;
    }

    bool ReadTextureDimensions(uint32_t rawHandle, float* width, float* height) {
        if (!rawHandle || !width || !height)
            return false;
        const hta::CMiracle3d* app = hta::CMiracle3d::Instance();
        hta::m3d::rend::IRenderer* renderer = app ? app->m_renderer : nullptr;
        if (!renderer)
            return false;

        const hta::m3d::rend::TexHandle handle(static_cast<int32_t>(rawHandle));
        int32_t pixelWidth = 0;
        int32_t pixelHeight = 0;
        renderer->GetDims(handle, pixelWidth, pixelHeight);
        if (pixelWidth <= 0 || pixelHeight <= 0)
            return false;
        *width = static_cast<float>(pixelWidth);
        *height = static_cast<float>(pixelHeight);
        return true;
    }

    void ReleaseTexture(uint32_t* rawHandle) {
        if (!rawHandle || !*rawHandle)
            return;

        const hta::CMiracle3d* app = hta::CMiracle3d::Instance();
        hta::m3d::rend::IRenderer* renderer = app ? app->m_renderer : nullptr;
        if (!renderer) {
            // The renderer can already be gone during shutdown. Do not retain
            // stale handles in the caller's state in that case.
            *rawHandle = 0;
            return;
        }

        hta::m3d::rend::TexHandle handle(static_cast<int32_t>(*rawHandle));
        renderer->ReleaseTexture(handle);
        *rawHandle = 0;
    }
}
