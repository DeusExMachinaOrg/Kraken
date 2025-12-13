#include "render/native/Image.hpp"

namespace kraken::render {
    bool Texture::Is2D() const {
        return this->mType <= TT_2D_RENDER_TARGET;
    };

    bool Texture::IsCube() const {
        return this->mType >= TT_CUBE_FROM_FILE && this->mType <= TT_CUBE_RENDER_TARGET;
    };

    bool Texture::Is3D() const {
        return this->mType >= TT_3D_FROM_FILE;
    };

    /// BORDER

    Texture::Texture() {
    };

    Texture::~Texture() {
        if (this->mHandleBase)
            this->mHandleBase->Release();
        this->mHandleBase = nullptr;
    };

    int32_t Texture::GetRef(void) {
        return this->mRefs;
    };

    int32_t Texture::AddRef(void) {
        return ++this->mRefs;
    };

    int32_t Texture::DecRef(void) {
        if (this->mRefs > 1)
            return --this->mRefs;
        delete this;
        return 0;
    };

    Texture::operator IDirect3DBaseTexture9* () const {
        return this->mHandleBase;
    };


    Sampler::Sampler() {
    };

    Sampler::~Sampler() {
        for (Texture* texture : this->m_maps)
            texture->DecRef();
        this->m_maps.clear();
    };

    int32_t Sampler::GetRef() {
        return this->mRefs;
    };

    int32_t Sampler::AddRef() {
        return ++this->mRefs;
    };

    int32_t Sampler::DecRef() {
        if (this->mRefs > 1)
            return --this->mRefs;
        delete this;
        return 0;
    };

    Sampler::operator IDirect3DBaseTexture9* () const {
        return *this->m_maps[0];
    };
};