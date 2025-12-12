#pragma once

#include <array>
#include <string>
#include <bitset>
#include <unordered_set>
#include <unordered_map>

#include <d3d9helper.h>
#include <D3DX9Mesh.h>
#include <d3d9.h>

#include "stdafx.hpp"
#include "render/d3d9/Common.hpp"

#include "hta/CVector2.hpp"
#include "hta/CVector.hpp"
#include "hta/CVector4.hpp"
#include "hta/CMatrix.hpp"
#include "hta/Quaternion.hpp"

namespace kraken::render::d3d9 {
    using f32vec2 = hta::CVector2;
    using f32vec3 = hta::CVector;
    using f32vec4 = hta::CVector4;
    using f32mat4 = hta::CMatrix;
    using f32quat = hta::Quaternion;

    class Texture;
    class Surface;
    class Uniform;

    enum class UniformType {
        UNKNOWN,
        VERTEX,
        FRAGMENT,
    };

    class Uniform {
    private:
        struct Range { int32_t min, max; };
    private:
        UniformType                           mTarget   { UniformType::UNKNOWN };
        IDirect3DDevice9*                     mOwner    { nullptr              };
        Range                                 mF32Range { 0, 0                 };
        std::array<float[4], 256>             mF32Data  {                      };
        Range                                 mI32Range { 0, 0                 };
        std::array<int32_t[4], 256>           mI32Data  {                      };
        std::bitset<8>                        mImgDirty { 0                    };
        std::array<IDirect3DBaseTexture9*, 8> mImgData  {                      };
    public:
        Uniform(UniformType target);
       ~Uniform();
    private:
        void CommitF32(void);
        void CommitI32(void);
        void CommitImg(void);
    public:
        void Attach(IDirect3DDevice9* owner);
        void Commit(void);
        void Reset(void);
        void Set(int32_t slot, bool v);
        void Set(int32_t slot, float v);
        void Set(int32_t slot, int32_t v);
        void Set(int32_t slot, f32vec2 v);
        void Set(int32_t slot, f32vec3 v);
        void Set(int32_t slot, f32vec4 v);
        void Set(int32_t slot, f32mat4 v);
        void Set(int32_t slot, const Surface* v);
        void Set(int32_t slot, const Texture* v);
        void Set(int32_t slot, const bool* v, int32_t slots);
        void Set(int32_t slot, const float* v, int32_t slots);
        void Set(int32_t slot, const int32_t* v, int32_t slots);
    public:
        Uniform(Uniform&&)      = delete;
        Uniform(const Uniform&) = delete;
    public:
        Uniform& operator = (Uniform&&)      = delete;
        Uniform& operator = (const Uniform&) = delete;
    };
};