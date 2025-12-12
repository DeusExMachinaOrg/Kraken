#include "render/d3d9/Shared.hpp"
#include "render/d3d9/Texture.hpp"

namespace kraken::render::d3d9 {
    Texture::operator IDirect3DBaseTexture9 *() const {
        return nullptr;
    };

    int32_t Texture2D::GetRef(void) {
        return this->mRefs;
    };

    int32_t Texture2D::AddRef(void) {
        return ++this->mRefs;
    };

    int32_t Texture2D::DecRef(void) {
        if (this->mRefs > 1)
            return --this->mRefs;
        delete this;
        return 0;
    };

    Texture2D::~Texture2D() {
        if (this->mObject)
            this->mObject->Release();
        this->mObject = nullptr;
    };

    Texture2D::operator IDirect3DBaseTexture9 *() const {
        return this->mObject;
    };

    int32_t Texture3D::GetRef(void) {
        return this->mRefs;
    };

    int32_t Texture3D::AddRef(void) {
        return ++this->mRefs;
    };

    int32_t Texture3D::DecRef(void) {
        if (this->mRefs > 1)
            return --this->mRefs;
        delete this;
        return 0;
    };

    Texture3D::~Texture3D(void) {
        if (this->mObject)
            this->mObject->Release();
        this->mObject = nullptr;
    };

    Texture3D::operator IDirect3DBaseTexture9 *() const {
        return this->mObject;
    };

    int32_t TextureCube::GetRef(void) {
        return this->mRefs;
    };

    int32_t TextureCube::AddRef(void) {
        return ++this->mRefs;
    };

    int32_t TextureCube::DecRef(void) {
        if (this->mRefs > 1)
            return --this->mRefs;
        delete this;
        return 0;
    };

    TextureCube::~TextureCube(void) {
        if (this->mObject)
            this->mObject->Release();
        this->mObject = nullptr;
    };

    TextureCube::operator IDirect3DBaseTexture9 *() const {
        return this->mObject;
    };

    int32_t Surface::GetRef(void) {
        return this->mRefs;
    };

    int32_t Surface::AddRef(void) {
        return ++this->mRefs;
    };

    int32_t Surface::DecRef(void) {
        if (this->mRefs > 1)
            return --this->mRefs;
        delete this;
        return 0;
    };

    Surface::Surface() {
    };

    Surface::~Surface(void) {
        for (Texture* texture : this->mTextures)
            texture->DecRef();
        this->mTextures.clear();
    };

    Surface::operator IDirect3DBaseTexture9 *() const {
        if (this->mTextures.empty())
            return nullptr;
        return *this->mTextures[0];
    };
};