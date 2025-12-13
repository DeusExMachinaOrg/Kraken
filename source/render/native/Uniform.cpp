#include "render/native/Shared.hpp"
#include "render/native/Uniform.hpp"
#include "render/native/Image.hpp"
#include "render/native/Debug.hpp"

namespace kraken::render {
    Uniform::Uniform(UniformType target): mTarget(target) {
        this->Reset();
    };

    Uniform::~Uniform() {
    };

    void Uniform::Attach(IDirect3DDevice9* owner) {
        this->mOwner = owner;
    };

    void Uniform::CommitF32(void) {
        if (this->mF32Range.min > this->mF32Range.max)
            return;

        size_t base  = this->mF32Range.min;
        size_t count = this->mF32Range.max - this->mF32Range.min + 1;

        if (this->mTarget == UniformType::VERTEX)
            this->mOwner->SetVertexShaderConstantF(base, this->mF32Data[base], count);
        else
            this->mOwner->SetPixelShaderConstantF(base, this->mF32Data[base], count);

        this->mF32Range.min = this->mF32Data.size();
        this->mF32Range.max = 0;
    };

    void Uniform::CommitI32(void) {
        if (this->mI32Range.min > this->mI32Range.max)
            return;

        size_t base  = this->mI32Range.min;
        size_t count = this->mI32Range.max - this->mI32Range.min + 1;

        if (this->mTarget == UniformType::VERTEX)
            this->mOwner->SetVertexShaderConstantI(base, this->mI32Data[base], count);
        else
            this->mOwner->SetPixelShaderConstantI(base, this->mI32Data[base], count);

        this->mI32Range.min = this->mI32Data.size();
        this->mI32Range.max = 0;
    };

    void Uniform::CommitImg(void) {
        if (!this->mImgDirty.any())
            return;

        for (size_t slot = 0; slot < this->mImgData.size(); slot++)
            if (this->mImgDirty[slot])
                this->mOwner->SetTexture(slot, this->mImgData[slot]);

        this->mImgDirty.reset();
    };

    void Uniform::Commit(void) {
        assert(this->mOwner && "Uniform class not attached to IDirect3DDevice9!");

        this->CommitF32();
        this->CommitI32();
        this->CommitImg();
    };

    void Uniform::Reset(void) {
        this->mF32Range.min = this->mF32Data.size();
        this->mF32Range.max = 0;
        this->mI32Range.min = this->mI32Data.size();
        this->mI32Range.max = 0;
        this->mImgDirty.reset();
    };

    void Uniform::Set(int32_t slot, float v) {
        float temp[4] = {v, 0.0f, 0.0f, 0.0f};

        if (memcmp(mF32Data[slot], temp, 16) == 0)
            return;

        memcpy(mF32Data[slot], temp, 16);

        if (slot < mF32Range.min) mF32Range.min = slot;
        if (slot > mF32Range.max) mF32Range.max = slot;
    };

    void Uniform::Set(int32_t slot, f32vec2 v) {
        float temp[4] = {v.x, v.y, 0.0f, 0.0f};

        if (memcmp(mF32Data[slot], temp, 16) == 0)
            return;

        memcpy(mF32Data[slot], temp, 16);

        if (slot < mF32Range.min) mF32Range.min = slot;
        if (slot > mF32Range.max) mF32Range.max = slot;
    };

    void Uniform::Set(int32_t slot, f32vec3 v) {
        float temp[4] = {v.x, v.y, v.z, 0.0f};

        if (memcmp(mF32Data[slot], temp, 16) == 0)
            return;

        memcpy(mF32Data[slot], temp, 16);

        if (slot < mF32Range.min) mF32Range.min = slot;
        if (slot > mF32Range.max) mF32Range.max = slot;
    };

    void Uniform::Set(int32_t slot, f32vec4 v) {
        if (memcmp(mF32Data[slot], &v.x, 16) == 0)
            return;

        memcpy(mF32Data[slot], &v.x, 16);

        if (slot < mF32Range.min) mF32Range.min = slot;
        if (slot > mF32Range.max) mF32Range.max = slot;
    };

    void Uniform::Set(int32_t slot, bool v) {
        int32_t temp[4] = {v ? 1 : 0, 0, 0, 0};

        if (memcmp(mI32Data[slot], temp, 16) == 0)
            return;

        memcpy(mI32Data[slot], temp, 16);

        if (slot < mI32Range.min) mI32Range.min = slot;
        if (slot > mI32Range.max) mI32Range.max = slot;
    };

    void Uniform::Set(int32_t slot, int32_t v) {
        int32_t temp[4] = {v, 0, 0, 0};

        if (memcmp(mI32Data[slot], temp, 16) == 0)
            return;

        memcpy(mI32Data[slot], temp, 16);

        if (slot < mI32Range.min) mI32Range.min = slot;
        if (slot > mI32Range.max) mI32Range.max = slot;
    };

    void Uniform::Set(int32_t slot, f32mat4 m) {
        bool changed = false;
        for (int i = 0; i < 4; i++) {
            if (memcmp(mF32Data[slot + i], &m.array[i * 4], 16) != 0) {
                changed = true;
                break;
            }
        }

        if (!changed)
            return;

        for (int i = 0; i < 4; i++) {
            memcpy(mF32Data[slot + i], &m.array[i * 4], 16);
        }

        if (slot < mF32Range.min) mF32Range.min = slot;
        if (slot + 3 > mF32Range.max) mF32Range.max = slot + 3;
    };

    void Uniform::Set(int32_t slot, const Sampler* surf) {
        IDirect3DBaseTexture9* d3dTex = surf ? *surf : nullptr;

        if (mImgData[slot] == d3dTex)
            return;

        mImgData[slot] = d3dTex;
        mImgDirty.set(slot);
    };

    void Uniform::Set(int32_t slot, const Texture* tex) {
        IDirect3DBaseTexture9* d3dTex = tex ? *tex : nullptr;

        if (mImgData[slot] == d3dTex)
            return;

        mImgData[slot] = d3dTex;
        mImgDirty.set(slot);
    };

    void Uniform::Set(int32_t slot, const float* v, int32_t slots) {
        bool changed = false;
        for (int32_t i = 0; i < slots; i++) {
            if (memcmp(mF32Data[slot + i], &v[i * 4], 16) != 0) {
                changed = true;
                break;
            }
        }

        if (!changed)
            return;

        for (int32_t i = 0; i < slots; i++) {
            memcpy(mF32Data[slot + i], &v[i * 4], 16);
        }

        if (slot < mF32Range.min) mF32Range.min = slot;
        if (slot + slots - 1 > mF32Range.max) mF32Range.max = slot + slots - 1;
    };

    void Uniform::Set(int32_t slot, const int32_t* v, int32_t slots) {
        bool changed = false;
        for (int32_t i = 0; i < slots; i++) {
            if (memcmp(mI32Data[slot + i], &v[i * 4], 16) != 0) {
                changed = true;
                break;
            }
        }

        if (!changed)
            return;

        for (int32_t i = 0; i < slots; i++) {
            memcpy(mI32Data[slot + i], &v[i * 4], 16);
        }
        
        if (slot < mI32Range.min) mI32Range.min = slot;
        if (slot + slots - 1 > mI32Range.max) mI32Range.max = slot + slots - 1;
    };

    void Uniform::Set(int32_t slot, const bool* v, int32_t slots) {
        bool changed = false;
        for (int32_t i = 0; i < slots; i++) {
            int32_t temp[4] = {
                v[i * 4    ] ? 1 : 0,
                v[i * 4 + 1] ? 1 : 0,
                v[i * 4 + 2] ? 1 : 0,
                v[i * 4 + 3] ? 1 : 0
            };

            if (memcmp(mI32Data[slot + i], temp, 16) != 0) {
                changed = true;
                memcpy(mI32Data[slot + i], temp, 16);
            }
        }

        if (!changed)
            return;

        if (slot < mI32Range.min) mI32Range.min = slot;
        if (slot + slots - 1 > mI32Range.max) mI32Range.max = slot + slots - 1;
    };
};