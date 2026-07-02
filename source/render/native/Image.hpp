#pragma once

#include "stdafx.hpp"

#include "hta/CStr.hpp"

#include "render/native/Shared.hpp"

#include <vector>

namespace kraken::render {
    enum TexType : int32_t {
        TT_2D_FROM_FILE       = 0x0000,
        TT_2D_DYNAMIC         = 0x0001,
        TT_2D_RENDER_TARGET   = 0x0002,
        TT_CUBE_FROM_FILE     = 0x0003,
        TT_CUBE_DYNAMIC       = 0x0004,
        TT_CUBE_RENDER_TARGET = 0x0005,
        TT_3D_FROM_FILE       = 0x0006,
        TT_3D_DYNAMIC         = 0x0007,
        TT_NUM_TYPES          = 0x0008,
    };

    struct Texture {
    public:
        enum eOrigin : uint32_t {
            eOrigin_UNKNOWN,
            eOrigin_FILESYSTEM,
            eOrigin_RUNTIME,
        };
        enum eDimension : uint32_t {
            eDimension_UNKNOWN,
            eDimension_1D,
            eDimension_2D,
            eDimension_3D,
            eDimension_CUBE,
        };
    public:
        int32_t    mRefs      { 0                  };
        eOrigin    mOrigin    { eOrigin_UNKNOWN    };
        eDimension mDimension { eDimension_UNKNOWN };
        hta::CStr  mFileName  {                    };
        uint32_t   mFileSize  { 0                  };
        FILETIME   mFileTime  {                    };
    public:
        uint32_t  mUsage;
        D3DPOOL   mPool;
        D3DFORMAT mFormat;
        uint32_t  mFlags;
        TexType   mType;
        union {
            D3DSURFACE_DESC mSurface2D;
            D3DVOLUME_DESC mSurface3D;
        };
        union {
            IDirect3DBaseTexture9* mHandleBase;
            IDirect3DTexture9* mHandle2D;
            IDirect3DCubeTexture9* mHandleCube;
            IDirect3DVolumeTexture9* mHandle3D;
        };

        bool Is2D() const;
        bool IsCube() const;
        bool Is3D() const;
    public:
        Texture();
       ~Texture();
    public:
        int32_t GetRef(void);
        int32_t AddRef(void);
        int32_t DecRef(void);
    public:
        operator IDirect3DBaseTexture9*() const;
    };

    struct Sampler {
    public:
        int32_t   mRefs     { 0 };
        hta::CStr mFileName {   };
    public:
        D3DTEXTUREADDRESS m_address[3];
        D3DTEXTUREFILTERTYPE m_magFilter;
        D3DTEXTUREFILTERTYPE m_minFilter;
        D3DTEXTUREFILTERTYPE m_mipFilter;
        uint32_t m_borderColor;
        int32_t m_maxAnisotropy;
        float m_lodBias;
        int32_t m_lodMax;
        int32_t m_fps;
        uint32_t m_looped;
        double m_timeStamp;
        std::vector<Texture*, std::allocator<Texture*>> m_maps;
    public:
        Sampler();
       ~Sampler();
    public:
        int32_t GetRef(void);
        int32_t AddRef(void);
        int32_t DecRef(void);
    public:
        operator IDirect3DBaseTexture9*() const;
    };
};