#pragma once

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <chrono>
#include <span>

#include "stdafx.hpp"

#include "render/d3d9/Shared.hpp"
#include "render/d3d9/Debug.hpp"
#include "render/d3d9/Common.hpp"

#include "hta/m3d/rend/TexHandle.hpp"

namespace kraken::render::d3d9 {
    static const uint32_t D3DUSAGE_DEFAULT = 0;
    using D3DUSAGE = uint32_t;

    class Texture;
    class Texture2D;
    class Texture3D;
    class TextureCube;
    class Surface;
    class ImageManager;

    enum class SurfaceType {
        UNKNOWN,
        IMAGE,
        VOLUME,
        CUBEMAP,
    };

    enum class SurfaceSource {
        RUNTIME,
        FILESYSTEM,
    };

    class Texture {
    protected:
        int32_t       mRefs          { 0                      };
        int32_t       mHandle        { -1                     };
        D3DUSAGE      mUsage         { D3DUSAGE_DEFAULT       };
        D3DPOOL       mPool          { D3DPOOL_DEFAULT        };
        D3DFORMAT     mFormat        { D3DFMT_A8R8G8B8        };
        std::string   mFilepath      {                        };
        double        mFiletime      {                        };
        SurfaceType   mSurfaceType   { SurfaceType::UNKNOWN   };
        SurfaceSource mSurfaceSource { SurfaceSource::RUNTIME };
    public:
        Texture() = default;
    public:
        Texture(Texture&&)      = delete;
        Texture(const Texture&) = delete;
    public:
        Texture& operator = (Texture&&)      = delete;
        Texture& operator = (const Texture&) = delete;
    public:
        virtual ~Texture() = default;
    public:
        virtual int32_t GetRef(void) = 0;
        virtual int32_t AddRef(void) = 0;
        virtual int32_t DecRef(void) = 0;
    public:
        virtual operator IDirect3DBaseTexture9* () const;
    };

    class Texture2D : public Texture {
    protected:
        D3DSURFACE_DESC    mDescriptor {         };
        IDirect3DTexture9* mObject     { nullptr };
    public:
        virtual int32_t GetRef(void) override;
        virtual int32_t AddRef(void) override;
        virtual int32_t DecRef(void) override;
    public:
        virtual ~Texture2D();
    public:
        virtual operator IDirect3DBaseTexture9* () const override;
    };

    class Texture3D : public Texture {
    protected:
        D3DVOLUME_DESC           mDescriptor {         };
        IDirect3DVolumeTexture9* mObject     { nullptr };
    public:
        virtual int32_t GetRef(void) override;
        virtual int32_t AddRef(void) override;
        virtual int32_t DecRef(void) override;
    public:
        virtual ~Texture3D();
    public:
        virtual operator IDirect3DBaseTexture9* () const override;
    };

    class TextureCube : public Texture {
    protected:
        D3DVOLUME_DESC         mDescriptor {         };
        IDirect3DCubeTexture9* mObject     { nullptr };
    public:
        virtual int32_t GetRef(void) override;
        virtual int32_t AddRef(void) override;
        virtual int32_t DecRef(void) override;
    public:
        virtual ~TextureCube();
    public:
        virtual operator IDirect3DBaseTexture9* () const override;
    };

    class Surface {
    private:
        int32_t               mRefs          { 0                };
        int32_t               mHandle        { -1               };
        D3DTEXTUREADDRESS     mWrapU         { D3DTADDRESS_WRAP };
        D3DTEXTUREADDRESS     mWrapV         { D3DTADDRESS_WRAP };
        D3DTEXTUREADDRESS     mWrapW         { D3DTADDRESS_WRAP };
        D3DTEXTUREFILTERTYPE  mFilterMin     { D3DTEXF_LINEAR   };
        D3DTEXTUREFILTERTYPE  mFilterMag     { D3DTEXF_LINEAR   };
        D3DTEXTUREFILTERTYPE  mFilterMip     { D3DTEXF_LINEAR   };
        u8color               mBorder        { 0, 0, 0, 255     };
        uint32_t              mMaxAnisotropy { 0                };
        int32_t               mLodBias       { 0                };
        int32_t               mLodCount      { 1                };
        int32_t               mFPS           { -1               };
        bool                  mLooped        { false            };
        double                mTimestamp     { 0.0              };
        std::string           mFilepath      {                  };
        std::vector<Texture*> mTextures      {                  };
    public:
        Surface();
       ~Surface();
    public:
        Surface(Surface&&)      = delete;
        Surface(const Surface&) = delete;
    public:
        Surface& operator = (Surface&&)      = delete;
        Surface& operator = (const Surface&) = delete;
    public:
        int32_t GetRef(void);
        int32_t AddRef(void);
        int32_t DecRef(void);
    public:
        operator IDirect3DBaseTexture9* () const;
    };
};