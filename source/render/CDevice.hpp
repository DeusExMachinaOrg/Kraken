#pragma once

#include "hta/CMatrix.hpp"
#include "hta/m3d/IDeviceResetCallback.hpp"
#include "hta/m3d/rend/Common.hpp"
#include "hta/m3d/rend/IRenderer.hpp"
#include "hta/m3d/rend/TexHandle.hpp"
#include "stdafx.hpp"

#include "render/helper.hpp"

#include <list>
#include <map>
#include <vector>
#include <array>

#include "render/d3d9/Shared.hpp"
#include "render/d3d9/Uniform.hpp"

namespace kraken::render {
    using namespace hta::m3d::rend;

    struct CDevice;

    enum CompileParam : int32_t {
        COMPILE_INSTANCING_VERSION = 0x0000,
        COMPILE_SUPPORT_SHADOWMAP  = 0x0001,
        NUM_COMPILE_PARAMS         = 0x0002,
    };

    enum StreamDataType : int32_t {
        M3DSDT_DEFAULT_DATA   = 0x0000,
        M3DSDT_INDEXED_DATA   = 0x0001,
        M3DSDT_INSTANCED_DATA = 0x0002,
    };

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

    struct StateManager : public ID3DXEffectStateManager {
        /* Size=0xc */
        /* 0x0000: fields for ID3DXEffectStateManager */
        /* 0x0004 */ CDevice* m_dev;
        /* 0x0008 */ uint32_t m_nRefs;

        StateManager();
        void SetDevice(CDevice*);
        virtual HRESULT __stdcall QueryInterface(const GUID&, void**) override;
        virtual ULONG __stdcall AddRef() override;
        virtual ULONG __stdcall Release() override;
        virtual HRESULT __stdcall SetTransform(D3DTRANSFORMSTATETYPE, const D3DMATRIX*) override;
        virtual HRESULT __stdcall SetMaterial(const D3DMATERIAL9*) override;
        virtual HRESULT __stdcall SetLight(DWORD, const D3DLIGHT9*) override;
        virtual HRESULT __stdcall LightEnable(DWORD, BOOL) override;
        virtual HRESULT __stdcall SetRenderState(D3DRENDERSTATETYPE, DWORD) override;
        virtual HRESULT __stdcall SetTexture(DWORD, LPDIRECT3DBASETEXTURE9) override;
        virtual HRESULT __stdcall SetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD) override;
        virtual HRESULT __stdcall SetSamplerState(DWORD, D3DSAMPLERSTATETYPE, DWORD) override;
        virtual HRESULT __stdcall SetNPatchMode(float) override;
        virtual HRESULT __stdcall SetFVF(DWORD) override;
        virtual HRESULT __stdcall SetVertexShader(IDirect3DVertexShader9*) override;
        virtual HRESULT __stdcall SetVertexShaderConstantF(uint32_t, const float*, uint32_t) override;
        virtual HRESULT __stdcall SetVertexShaderConstantI(uint32_t, const int32_t*, uint32_t) override;
        virtual HRESULT __stdcall SetVertexShaderConstantB(uint32_t, const int32_t*, uint32_t) override;
        virtual HRESULT __stdcall SetPixelShader(IDirect3DPixelShader9*) override;
        virtual HRESULT __stdcall SetPixelShaderConstantF(uint32_t, const float*, uint32_t) override;
        virtual HRESULT __stdcall SetPixelShaderConstantI(uint32_t, const int32_t*, uint32_t) override;
        virtual HRESULT __stdcall SetPixelShaderConstantB(uint32_t, const int32_t*, uint32_t) override;
    };

    struct nShaderArg {
        enum Type : int32_t {
            Void     = 0x0000,
            Bool     = 0x0001,
            Int      = 0x0002,
            Float    = 0x0003,
            Float4   = 0x0004,
            Matrix44 = 0x0005,
            Texture  = 0x0006,
        };
        /* Size=0x44 */
        /* 0x0000 */ Type m_type;
        union {
            /* 0x0004 */ bool b;
            /* 0x0004 */ int32_t i;
            /* 0x0004 */ float f;
            /* 0x0004 */ TexHandle tex;
            /* 0x0004 */ hta::nFloat4 f4;
            /* 0x0004 */ float m[4][4];
        };

        nShaderArg();
        ~nShaderArg();
        bool operator==(const nShaderArg&) const;
        void SetType(Type);
        Type GetType() const;
        void SetBool(bool);
        bool GetBool() const;
        void SetInt(int32_t);
        int32_t GetInt() const;
        void SetFloat(float);
        float GetFloat() const;
        void SetFloat4(const hta::nFloat4&);
        const hta::nFloat4& GetFloat4() const;
        void SetMatrix44(const hta::CMatrix*);
        const hta::CMatrix* GetMatrix44() const;
        void SetTexture(TexHandle*);
        TexHandle* GetTexture();
    };

    struct nShaderParams {
        static inline const size_t SIZE = 49;
        /* Size=0xd38 */
        /* 0x0000 */ bool m_valid[SIZE];
        /* 0x0034 */ nShaderArg m_args[SIZE];

        nShaderParams();
        ~nShaderParams();
        bool IsParameterValid(IEffect::Parameter) const;
        void SetArg(IEffect::Parameter, const nShaderArg&);
        const nShaderArg& GetArg(IEffect::Parameter) const;
        void SetInt(IEffect::Parameter, int32_t);
        int32_t GetInt(IEffect::Parameter) const;
        void SetFloat(IEffect::Parameter, float);
        float GetFloat(IEffect::Parameter) const;
        void SetFloat4(IEffect::Parameter, const hta::nFloat4&);
        const hta::nFloat4& GetFloat4(IEffect::Parameter) const;
        void SetMatrix44(IEffect::Parameter, const hta::CMatrix*);
        const hta::CMatrix* GetMatrix44(IEffect::Parameter) const;
        void SetTexture(IEffect::Parameter, TexHandle);
        TexHandle GetTexture(IEffect::Parameter);
        void SetVector4(IEffect::Parameter, const hta::CVector4&);
        hta::CVector4 GetVector4(IEffect::Parameter) const;
        void Reset();
    };

    class auxShaderInclude : public ID3DXInclude { /* Size=0x4 */
        /* 0x0000: fields for ID3DXInclude */

        virtual HRESULT __stdcall Open(D3DXINCLUDE_TYPE, const char*, const void*, const void**, uint32_t*) override;
        virtual HRESULT __stdcall Close(const void*) override;
    };

    struct EffectImpl : public IEffect {
        struct auxParamDesc {
            /* Size=0xc */
            /* 0x0000 */ int32_t space;
            /* 0x0004 */ bool isShared;
            /* 0x0008 */ const char* d3dxHandle;
        };

        struct UserParam {
            enum UserParamType : int32_t {
                UPT_MATRIX  = 0x0000,
                UPT_VECTOR4 = 0x0001,
                UPT_VECTOR  = 0x0002,
                UPT_FLOAT   = 0x0003,
            };
            /* Size=0x10 */
            /* 0x0000 */ hta::CStr name;
            /* 0x000c */ UserParamType type;
        };

        struct TechniqueDesc : IEffect::TechniqueDesc {
            /* 0x0030 */ int32_t maxInstances;
            /* 0x0034 */ std::vector<UserParam> userParams;
        };

        struct TechniqueDescInternal {
            /* Size=0x48 */
            /* 0x0000 */ TechniqueDesc publicDesc;
            /* 0x0044 */ const char* handle;
        };

        /* Size=0xfdc */
        /* 0x0000: fields for IEffect */
        /* 0x0008 */ bool m_bApplyGlobalParams{false};
        /* 0x000c */ hta::CStr m_fileName{};
        /* 0x0018 */ std::vector<CompileParam> m_compileParams{};
        /* 0x0028 */ ID3DXEffect* m_effect{nullptr};
        /* 0x002c */ bool m_hasBeenValidated{false};
        /* 0x002d */ bool m_didNotValidate{false};
        /* 0x0030 */ auxParamDesc m_parameterHandles[49]{};
        /* 0x027c */ nShaderParams m_curParams{};
        /* 0x0fb4 */ uint32_t m_numTechniques{0};
        /* 0x0fb8 */ int32_t m_curTechnique{-1};
        /* 0x0fbc */ std::vector<TechniqueDescInternal> m_techDescs{};
        /* 0x0fcc */ int32_t m_defaultTechnique{-1};
        /* 0x0fd0 */ int32_t m_defaultPS20Technique{-1};
        /* 0x0fd4 */ uint32_t m_numPrimitives{0};
        /* 0x0fd8 */ uint32_t m_numDIPs{};

        EffectImpl(const EffectImpl&);
        EffectImpl(bool);
        virtual ~EffectImpl();
        virtual bool LoadFromFile(const char*, const std::vector<enum CompileParam, std::allocator<enum CompileParam>>&);
        virtual bool LoadFromString(const char*, uint32_t);
        virtual bool IsValid() const;
        virtual uint32_t GetNumTechniques() const;
        virtual const IEffect::TechniqueDesc& GetTechniqueDesc(uint32_t) const;
        virtual void SetCurTechnique(uint32_t);
        virtual void SetCurTechniqueByName(const char*);
        virtual uint32_t GetCurTechnique() const;
        virtual const char* GetCurTechniqueName() const;
        virtual void SetDefaultTechnique(bool) override;
        virtual bool IsParameterUsed(IEffect::Parameter) override;
        virtual void SetInt(IEffect::Parameter, int32_t) override;
        virtual void SetFloat(IEffect::Parameter, float) override;
        virtual void SetVector4(IEffect::Parameter, const hta::CVector4&) override;
        virtual void SetVector3(IEffect::Parameter, const hta::CVector&) override;
        virtual void SetFloat4(IEffect::Parameter, const hta::nFloat4&) override;
        virtual void SetMatrix(IEffect::Parameter, const hta::CMatrix&) override;
        virtual void SetTexture(IEffect::Parameter, const TexHandle&) override;
        virtual void SetIntArray(IEffect::Parameter, const int32_t*, int32_t) override;
        virtual void SetFloatArray(IEffect::Parameter, const float*, int32_t) override;
        virtual void SetFloat4Array(IEffect::Parameter, const hta::nFloat4*, int32_t) override;
        virtual void SetVector4Array(IEffect::Parameter, const hta::CVector4*, int32_t) override;
        virtual void SetMatrixArray(IEffect::Parameter, const hta::CMatrix*, int32_t) override;
        virtual void SetMatrixPointerArray(IEffect::Parameter, const hta::CMatrix**, int32_t) override;
        virtual void SetParams(nShaderParams&);
        int32_t Begin();
        void BeginPass(uint32_t);
        void EndPass();
        void End();
        void CommitChanges();
        void ApplyGlobalFxParams();
        void OnDeviceReset();
        void OnDeviceRestore();
        void Invalidate();
        void ValidateEffect();
        void UpdateParameterHandles();
        bool getBoolAnnotation(const char*, const char*, bool);
        int32_t getIntAnnotation(const char*, const char*, int32_t);
        hta::CStr getStringAnnotation(const char*, const char*, const hta::CStr&);
        void getUserRenderParams(const hta::CStr&, std::vector<UserParam>&);
    };

    struct PoolFieldInfo {
        /* Size=0x8 */
        /* 0x0000 */ uint32_t Size;
        /* 0x0004 */ uint32_t Offset;

        bool operator==(const PoolFieldInfo&);
    };

    struct MeshHandle : public Handle<MeshHandle> {
        /* Size=0x4 */
        /* 0x0000: fields for Handle<MeshHandle> */
    };

    struct HlslShaderImpl : public IHlslShader {
        /* Size=0x44 */
        /* 0x0000: fields for IHlslShader */
        /* 0x0008 */ uint32_t m_numConstants{0};
        /* 0x000c */ hta::CStr m_fileName{};
        /* 0x0018 */ std::vector<CompileParam> m_compileParams{};
        /* 0x0028 */ hta::CStr m_entryPoint{};
        /* 0x0034 */ IHlslShader::Profile m_profile{};
        union {
            /* 0x0038 */ void* m_shader;
            /* 0x0038 */ IDirect3DVertexShader9* m_vs;
            /* 0x0038 */ IDirect3DPixelShader9* m_ps;
        };
        /* 0x003c */ ID3DXConstantTable* m_constantTable;
        /* 0x0040 */ uint32_t m_numPrimitives;

        virtual ~HlslShaderImpl();
        virtual bool LoadFromFile(const char*, const char*, IHlslShader::Profile, const std::vector<CompileParam>&);
        virtual bool LoadFromString(const char*, uint32_t, const char*, IHlslShader::Profile);
        virtual bool IsValid() const;
        virtual uint32_t GetNumberOfParams() const;
        virtual uint32_t GetParamHandleByName(const char*);
        virtual void SetInt(uint32_t, int32_t);
        virtual void SetFloat(uint32_t, float);
        virtual void SetVector4(uint32_t, const hta::CVector4&);
        virtual void SetVector3(uint32_t, const hta::CVector&);
        virtual void SetFloat4(uint32_t, const hta::nFloat4&);
        virtual void SetMatrix(uint32_t, const hta::CMatrix&);
        virtual void SetIntArray(uint32_t, const int32_t*, int32_t);
        virtual void SetFloatArray(uint32_t, const float*, int32_t);
        virtual void SetFloat4Array(uint32_t, const hta::nFloat4*, int32_t);
        virtual void SetVector4Array(uint32_t, const hta::CVector4*, int32_t);
        virtual void SetMatrixArray(uint32_t, const hta::CMatrix*, int32_t);
        virtual void SetMatrixPointerArray(uint32_t, const hta::CMatrix**, int32_t);
        virtual void Apply();
        void UpdateShaderInfo();
        bool IsValidParam(uint32_t) const;
        void OnDeviceReset();
        void OnDeviceRestore();
        void Invalidate();
        void ValidateEffect();
        bool IsVertexShader() const;
    };

    struct AsmShaderImpl : public IAsmShader {
        /* Size=0x1c */
        /* 0x0000: fields for IAsmShader */
        /* 0x0008 */ hta::CStr m_fileName{};
        /* 0x0014 */ IAsmShader::Type m_type{};
        union {
            /* 0x0018 */ void* m_shader;
            /* 0x0018 */ IDirect3DVertexShader9* m_vs;
            /* 0x0018 */ IDirect3DPixelShader9* m_ps;
        };

        AsmShaderImpl() = default;
        virtual ~AsmShaderImpl();
        virtual bool LoadFromFile(const char*, IAsmShader::Type);
        virtual bool LoadFromString(const char*, uint32_t, IAsmShader::Type);
        virtual bool IsValid() const;
        virtual void Apply() override;
        void OnDeviceReset();
        void OnDeviceRestore();
        void Invalidate();
        bool IsVertexShader() const;
    };

    struct SubmeshInfo {
        /* Size=0x10 */
        /* 0x0000 */ int32_t m_firstVert;
        /* 0x0004 */ int32_t m_numVerts;
        /* 0x0008 */ int32_t m_firstTri;
        /* 0x000c */ int32_t m_numTris;
    };

    enum ImageFileFormats : int32_t {
        M3DIFF_BMP = 0x0000,
        M3DIFF_JPG = 0x0001,
        M3DIFF_TGA = 0x0002,
        M3DIFF_PNG = 0x0003,
        M3DIFF_DDS = 0x0004,
        M3DIFF_PPM = 0x0005,
        M3DIFF_DIB = 0x0006,
        M3DIFF_HDR = 0x0007,
        M3DIFF_PFM = 0x0008,
    };

    struct Query : public IQuery {
        /* Size=0x38 */
        /* 0x0000: fields for m3d::rend::IQuery */
        /* 0x0008 */ IQuery::Type m_type;
        /* 0x000c */ IQuery::State m_state;
        /* 0x0010 */ IDirect3DQuery9* m_query;
        /* 0x0018 */ QueryReturnValue m_retVal;

        Query(const Query&);
        Query(IQuery::Type);
        int32_t CreateQuery();
        void ReleaseQuery();
        virtual IQuery::Type GetType() const;
        virtual void Begin();
        virtual void End();
        virtual IQuery::State GetState();
        virtual bool IsValid() const;
        virtual const QueryReturnValue& GetData();
        virtual ~Query();
    };

    struct CDevice : IRenderer {
        struct CVertexBuffer {
            /* Size=0x40 */
            /* 0x0000 */ IDirect3DVertexBuffer9* m_vb{nullptr};
            /* 0x0004 */ D3DVERTEXBUFFER_DESC m_desc{};
            /* 0x001c */ IDirect3DVertexDeclaration9* m_vertexDecl{nullptr};
            /* 0x0020 */ int32_t m_vertSz{0};
            /* 0x0024 */ int32_t m_curPos{0};
            /* 0x0028 */ int32_t m_refs{0};
            /* 0x002c */ int32_t m_locked{0};
            /* 0x0030 */ int32_t m_lockedAtPresent{0};
            /* 0x0034 */ hta::CStr m_vbName{};
                         VertexType m_vtType{};
            CVertexBuffer() = default;
        };
        struct CIndexBuffer {
            /* Size=0x24 */
            /* 0x0000 */ IDirect3DIndexBuffer9* m_ib{nullptr};
            /* 0x0004 */ D3DINDEXBUFFER_DESC m_desc{};
            /* 0x0018 */ int32_t m_curPos{0};
            /* 0x001c */ int32_t m_refs{0};
            /* 0x0020 */ int32_t m_locked{0};
        };
        struct ShaderIdData {
            /* Size=0x1c */
            /* 0x0000 */ hta::CStr filename;
            /* 0x000c */ std::vector<CompileParam> compileParams;
            bool operator<(const ShaderIdData&) const;
            ShaderIdData() = default;
        };
        struct CImage {
            /* Size=0x54 */
            /* 0x0000 */ uint32_t m_usage;
            /* 0x0004 */ D3DPOOL m_pool;
            /* 0x0008 */ D3DFORMAT m_fmt;
            /* 0x000c */ uint32_t m_flags;
            union {
                /* 0x0010 */ D3DSURFACE_DESC m_desc2d;
                /* 0x0010 */ D3DVOLUME_DESC m_desc3d;
            };
            union {
                /* 0x0030 */ IDirect3DBaseTexture9* m_pTex;
                /* 0x0030 */ IDirect3DTexture9* m_pTex2d;
                /* 0x0030 */ IDirect3DCubeTexture9* m_pTexCube;
                /* 0x0030 */ IDirect3DVolumeTexture9* m_pTex3d;
            };
            /* 0x0034 */ TexType m_type;
            /* 0x0038 */ hta::CStr m_fileName;
            /* 0x0044 */ uint32_t m_lastFileSize;
            /* 0x0048 */ _FILETIME m_lastFileDate;
            /* 0x0050 */ int32_t m_refs;

            CImage() = default;
            void freeTex();
            int32_t addRef();
            int32_t release();
            bool Is2D() const;
            bool IsCube() const;
            bool Is3D() const;
        };
        struct CTexture {
            /* Size=0x60 */
            /* 0x0000 */ D3DTEXTUREADDRESS m_address[3];
            /* 0x000c */ D3DTEXTUREFILTERTYPE m_magFilter;
            /* 0x0010 */ D3DTEXTUREFILTERTYPE m_minFilter;
            /* 0x0014 */ D3DTEXTUREFILTERTYPE m_mipFilter;
            /* 0x0018 */ uint32_t m_borderColor;
            /* 0x001c */ int32_t m_maxAnisotropy;
            /* 0x0020 */ float m_lodBias;
            /* 0x0024 */ int32_t m_lodMax;
            /* 0x0028 */ int32_t m_refs;
            /* 0x002c */ int32_t m_fps;
            /* 0x0030 */ uint32_t m_looped;
            /* 0x0038 */ double m_timeStamp;
            /* 0x0040 */ hta::CStr m_fileName;
            /* 0x004c */ std::vector<CImage*, std::allocator<CImage*>> m_maps;

            CTexture() = default;
            int32_t addRef();
            int32_t release();
        };
        struct CMesh {
            /* Size=0x20 */
            /* 0x0000 */ ID3DXMesh* m_mesh{nullptr};
            /* 0x0004 */ ID3DXPMesh* m_pmesh{nullptr};
            /* 0x0008 */ int32_t m_refs{0};
            /* 0x000c */ void* m_verts{nullptr};
            /* 0x0010 */ int32_t m_numVerts{0};
            /* 0x0014 */ uint16_t* m_indices{nullptr};
            /* 0x0018 */ int32_t m_numTris{0};
            /* 0x001c */ VertexType m_vertexType{};
            CMesh() = default;
        };
        struct MacroData {
            /* Size=0x1c */
            /* 0x0000 */ ShaderMacro macro;
            /* 0x0018 */ bool userDefined;
        };
        struct DXCursorInfo {
            /* Size=0x10 */
            /* 0x0000 */ TexHandle m_texId{-1};
            /* 0x0004 */ int32_t m_xHotSpot{0};
            /* 0x0008 */ int32_t m_yHotSpot{0};
            /* 0x000c */ int32_t m_frame{0};

            void SetUp(const TexHandle&, int32_t, int32_t, int32_t);
        };

        d3d9::Uniform mVSUniforms { d3d9::UniformType::VERTEX   };
        d3d9::Uniform mFSUniforms { d3d9::UniformType::FRAGMENT };

        Registry<CTexture>                     mActiveTextures {};
        Registry<CImage>                       mActiveImages   {};
        std::array<IDirect3DVertexBuffer9*, 8> mBindedStreams  {};

        CTexture* CreateTexture(int32_t& handle);
        void      DeleteTexture(int32_t handle);

        /* Size=0xf1ec */
        /* 0x0000: fields for IRenderer */
        /* 0x0004 */ int32_t m_refCount{0};
        /* 0x0008 */ IBase* m_parent{nullptr};
        /* 0x000c */ int32_t m_presents{0};
        /* 0x0010 */ std::vector<CVertexBuffer> m_vbs;
        /* 0x0020 */ std::vector<VertexType> m_VbPoolTypes;
        /* 0x0030 */ std::vector<std::vector<VbHandle>> m_VbPoolBuffers;
        /* 0x0040 */ std::vector<std::list<PoolFieldInfo>> m_VbPoolFields;
        /* 0x0050 */ const uint32_t m_VbPoolSize{0x10000};
        /* 0x0054 */ std::vector<CIndexBuffer> m_ibs;
        /* 0x0064 */ std::vector<IbHandle> m_IbPoolBuffers;
        /* 0x0074 */ std::list<PoolFieldInfo, std::allocator<PoolFieldInfo>> m_IbPoolFields;
        /* 0x0080 */ const uint32_t m_IbPoolSize{0x10000};
        //           std::vector<CImage*> m_texMaps;
        //           std::map<int32_t, CTexture*> m_textures;
        //           std::vector<int32_t> m_texturesSlots;
        /* 0x00a4 */ TexHandle m_texFrameBufer;
        /* 0x00a8 */ std::map<int, TexHandle> m_texBufferedRT;
        /* 0x00b4 */ std::vector<CMesh, std::allocator<CMesh>> m_meshes;
        /* 0x00c4 */ D3DPRESENT_PARAMETERS m_d3dpp;
        /* 0x00fc */ IDirect3D9* m_pD3D{nullptr};
        /* 0x0100 */ IDirect3DDevice9* m_pd3dDevice{nullptr};
        /* 0x0104 */ D3DCAPS9 m_d3dCaps;
        /* 0x0234 */ D3DSURFACE_DESC m_d3dsdBackBuffer;
        /* 0x0254 */ D3DDISPLAYMODE m_desktopMode;
        /* 0x0264 */ D3DFORMAT m_surfaceFormat;
        /* 0x0268 */ D3DFORMAT m_texFormat[2][2];
        /* 0x0278 */ D3DFORMAT m_texFormatRt;
        /* 0x027c */ D3DFORMAT m_texFormatShadow;
        /* 0x0280 */ D3DFORMAT m_texFormatDepth;
        /* 0x0284 */ D3DFORMAT m_depthStencilFormat;
        /* 0x0288 */ D3DFORMAT m_depthStencilFormatRt;
        /* 0x028c */ IDirect3DSurface9* m_rtsPtr{nullptr};
        /* 0x0290 */ IDirect3DSurface9* m_rtsSaveZs{nullptr};
        /* 0x0294 */ IDirect3DSurface9* m_rtsSaveColor{nullptr};
        /* 0x0298 */ IDirect3DSurface9* m_rtsNewZs{nullptr};
        /* 0x029c */ std::map<unsigned int, IDirect3DSurface9*> m_rtsZSurfaces;
        /* 0x02a8 */ int32_t m_rtsWantZ;
        /* 0x02ac */ Viewport m_rtsSaveViewport;
        /* 0x02c4 */ bool m_haveStencil;
        /* 0x02c5 */ bool m_haveTwoSidedStencil;
        /* 0x02c6 */ D3DGAMMARAMP m_gamma;
        /* 0x08c8 */ hta::CMatrix m_matViewStack[64];
        /* 0x18c8 */ hta::CMatrix m_matInvView;
        /* 0x1908 */ int32_t m_matViewStackTop;
        /* 0x190c */ bool m_matViewIsNotActuated;
        /* 0x190d */ bool m_matInvViewIsNotActuated;
        /* 0x190e */ bool m_matWorldIsNotActuated;
        /* 0x1910 */ hta::CMatrix m_matProjStack[64];
        /* 0x2910 */ int32_t m_matProjStackTop;
        /* 0x2914 */ hta::CMatrix m_matWorldStack[64];
        /* 0x3914 */ int32_t m_matWorldStackTop;
        /* 0x3918 */ Viewport m_curViewport;
        /* 0x3930 */ D3DVIEWPORT9 m_curViewportD3D;
        /* 0x3948 */ std::vector<hta::m3d::IDeviceResetCallback*> m_resetCallbacks;
        /* 0x3958 */ int32_t m_curFVF{-1};
        /* 0x395c */ IDirect3DVertexDeclaration9* m_curVertexDecl{nullptr};
        /* 0x3960 */ IDirect3DVertexBuffer9* m_curVb[4];
        /* 0x3970 */ uint32_t m_curStride[4];
        /* 0x3980 */ IDirect3DIndexBuffer9* m_curIb{nullptr};
        /* 0x3984 */ uint32_t m_curIbBaseIdx;
        /* 0x3988 */ IDirect3DBaseTexture9* m_curTexStages[8];
        /* 0x39a8 */ uint32_t m_curTexStagesStates[8][33];
        /* 0x3dc8 */ uint32_t m_curTexSamplerStates[8][14];
        /* 0x3f88 */ uint32_t m_curStreamFreq[8];
        /* 0x3fa8 */ uint32_t m_curRenderState[210];
        /* 0x42f0 */ IDirect3DIndexBuffer9* m_latchedIb{nullptr};
        /* 0x42f4 */ uint32_t m_latchedIbBaseIdx;
        /* 0x42f8 */ hta::CMatrix m_curXForms[512];
        /* 0xc2f8 */ bool m_latchedCheck{true};
        /* 0xc2fc */ IDirect3DVertexShader9* m_curVertexShader{nullptr};
        /* 0xc300 */ IDirect3DPixelShader9* m_curPixelShader{nullptr};
        /* 0xc304 */ IDirect3DCubeTexture9* m_NormalizingCubemap{nullptr};
        /* 0xc308 */ IDirect3DTexture9* m_SpecularPowerLookup{nullptr};
        /* 0xc30c */ IDirect3DVertexDeclaration9* m_vdXYZCT1{nullptr};
        /* 0xc310 */ IDirect3DVertexDeclaration9* m_vdXYZT1{nullptr};
        /* 0xc314 */ IDirect3DVertexDeclaration9* m_vdXYZNT1{nullptr};
        /* 0xc318 */ IDirect3DVertexDeclaration9* m_vdXYZNT2{nullptr};
        /* 0xc31c */ IDirect3DVertexDeclaration9* m_vdXYZNT3{nullptr};
        /* 0xc320 */ IDirect3DVertexDeclaration9* m_vdXYZN{nullptr};
        /* 0xc324 */ IDirect3DVertexDeclaration9* m_vdXYZ{nullptr};
        /* 0xc328 */ IDirect3DVertexDeclaration9* m_vdXYZW{nullptr};
        /* 0xc32c */ IDirect3DVertexDeclaration9* m_vdXYZC{nullptr};
        /* 0xc330 */ IDirect3DVertexDeclaration9* m_vdXYZNC{nullptr};
        /* 0xc334 */ IDirect3DVertexDeclaration9* m_vdXYZWCT1{nullptr};
        /* 0xc338 */ IDirect3DVertexDeclaration9* m_vdXYZWC{nullptr};
        /* 0xc33c */ IDirect3DVertexDeclaration9* m_vdXYZNCT1{nullptr};
        /* 0xc340 */ IDirect3DVertexDeclaration9* m_vdXYZNCT2{nullptr};
        /* 0xc344 */ IDirect3DVertexDeclaration9* m_vdXYZCT2{nullptr};
        /* 0xc348 */ IDirect3DVertexDeclaration9* m_vdXYZW4NCT1{nullptr};
        /* 0xc34c */ IDirect3DVertexDeclaration9* m_vdXYZW4TNCT1{nullptr};
        /* 0xc350 */ IDirect3DVertexDeclaration9* m_vdXYZNT1T{nullptr};
        /* 0xc354 */ IDirect3DVertexDeclaration9* m_vdXYZNCT1T{nullptr};
        /* 0xc358 */ IDirect3DVertexDeclaration9* m_vdXYZCT1_UVW{nullptr};
        /* 0xc35c */ IDirect3DVertexDeclaration9* m_vdXYZCT2_UVW{nullptr};
        /* 0xc360 */ IDirect3DVertexDeclaration9* m_vdXYZNCT1_UV2_S1{nullptr};
        /* 0xc364 */ IDirect3DVertexDeclaration9* m_vdWaterTest{nullptr};
        /* 0xc368 */ IDirect3DVertexDeclaration9* m_vdGrassTest{nullptr};
        /* 0xc36c */ IDirect3DVertexDeclaration9* m_vdImpostorTest{nullptr};
        /* 0xc370 */ IDirect3DVertexDeclaration9* m_vdYNI{nullptr};
        /* 0xc374 */ IDirect3DVertexDeclaration9* m_vdXYZT1I{nullptr};
        /* 0xc378 */ IDirect3DVertexDeclaration9* m_vdInstanceId{nullptr};
        /* 0xc37c */ std::map<std::pair<IDirect3DVertexDeclaration9*, IDirect3DVertexDeclaration9*>, IDirect3DVertexDeclaration9*>
            m_combinedVD;
        /* 0xc388 */ int32_t m_maxLights;
        /* 0xc38c */ int32_t m_inScene{0};
        /* 0xc390 */ RenderStats m_stats;
        /* 0xc3c0 */ DeviceMemStats m_devMemStats;
        /* 0xc400 */ bool m_isDevMemStatsValid;
        /* 0xc404 */ HRESULT m_lastResult{0};
        /* 0xc408 */ int32_t m_isActive{0};
        /* 0xc40c */ int32_t m_stackAlphaTest[64];
        /* 0xc50c */ uint32_t m_stackTopAlphaTest{0};
        /* 0xc510 */ BlendMode m_stackBlend[64];
        /* 0xc610 */ uint32_t m_stackTopBlend{0};
        /* 0xc614 */ ZbState m_stackZbState[64];
        /* 0xc714 */ uint32_t m_stackTopZbState{0};
        /* 0xc718 */ Cull m_stackCull[64];
        /* 0xc818 */ uint32_t m_stackTopCull{0};
        /* 0xc81c */ CmpFunc m_stackZFunc[64];
        /* 0xc91c */ uint32_t m_stackTopZFunc{0};
        /* 0xc920 */ bool m_stackLighting[64];
        /* 0xc960 */ uint32_t m_stackTopLighting{0};
        /* 0xc964 */ uint32_t m_stackAmbient[64];
        /* 0xca64 */ uint32_t m_stackTopAmbient{0};
        /* 0xca68 */ bool m_stackFog[64];
        /* 0xcaa8 */ uint32_t m_stackTopFog{0};
        /* 0xcaac */ uint32_t m_stackFogColor[64];
        /* 0xcbac */ uint32_t m_stackTopFogColor{0};
        /* 0xcbb0 */ FogMode m_stackFogMode[64];
        /* 0xccb0 */ uint32_t m_stackTopFogMode{0};
        /* 0xccb4 */ float m_stackFogStart[64];
        /* 0xcdb4 */ uint32_t m_stackTopFogStart{0};
        /* 0xcdb8 */ float m_stackFogEnd[64];
        /* 0xceb8 */ uint32_t m_stackTopFogEnd{0};
        /* 0xcebc */ FillMode m_stackFillMode[64];
        /* 0xcfbc */ uint32_t m_stackTopFillMode{0};
        /* 0xcfc0 */ float m_stackZBias[64];
        /* 0xd0c0 */ uint32_t m_stackTopZBias{0};
        /* 0xd0c4 */ float m_stackZBiasSlopeScale[64];
        /* 0xd1c4 */ uint32_t m_stackTopZBiasSlopeScale{0};
        /* 0xd1c8 */ ShadeMode m_stackShadeMode[64];
        /* 0xd2c8 */ uint32_t m_stackTopShadeMode{0};
        /* 0xd2cc */ int32_t m_stackPointSpriteEnable[64];
        /* 0xd3cc */ uint32_t m_stackTopPointSpriteEnable{0};
        /* 0xd3d0 */ int32_t m_stackPointScaleEnable[64];
        /* 0xd4d0 */ uint32_t m_stackTopPointScaleEnable{0};
        /* 0xd4d4 */ float m_stackPointSizeMin[64];
        /* 0xd5d4 */ uint32_t m_stackTopPointSizeMin{0};
        /* 0xd5d8 */ float m_stackPointSizeMax[64];
        /* 0xd6d8 */ uint32_t m_stackTopPointSizeMax{0};
        /* 0xd6dc */ float m_stackPointSize[64];
        /* 0xd7dc */ uint32_t m_stackTopPointSize{0};
        /* 0xd7e0 */ float m_stackPointScaleA[64];
        /* 0xd8e0 */ uint32_t m_stackTopPointScaleA{0};
        /* 0xd8e4 */ float m_stackPointScaleB[64];
        /* 0xd9e4 */ uint32_t m_stackTopPointScaleB{0};
        /* 0xd9e8 */ float m_stackPointScaleC[64];
        /* 0xdae8 */ uint32_t m_stackTopPointScaleC{0};
        /* 0xdaec */ uint32_t m_stackTFactor[64];
        /* 0xdbec */ uint32_t m_stackTopTFactor{0};
        /* 0xdbf0 */ bool m_stackLocalViewer[64];
        /* 0xdc30 */ uint32_t m_stackTopLocalViewer{0};
        /* 0xdc34 */ bool m_stackSpecularLighting[64];
        /* 0xdc74 */ uint32_t m_stackTopSpecularLighting{0};
        /* 0xdc78 */ uint32_t m_stackColorWriteMask[64];
        /* 0xdd78 */ uint32_t m_stackTopColorWriteMask{0};
        /* 0xdd7c */ bool m_stackDithering[64];
        /* 0xddbc */ uint32_t m_stackTopDithering{0};
        /* 0xddc0 */ float m_stackNPatchLevel[64];
        /* 0xdec0 */ uint32_t m_stackTopNPatchLevel{0};
        /* 0xdec4 */ bool m_stackStencilState[64];
        /* 0xdf04 */ uint32_t m_stackTopStencilState{0};
        /* 0xdf08 */ uint32_t m_stackStencilMask[64];
        /* 0xe008 */ uint32_t m_stackTopStencilMask{0};
        /* 0xe00c */ uint32_t m_stackStencilRef[64];
        /* 0xe10c */ uint32_t m_stackTopStencilRef{0};
        /* 0xe110 */ uint32_t m_stackStencilWriteMask[64];
        /* 0xe210 */ uint32_t m_stackTopStencilWriteMask{0};
        /* 0xe214 */ CmpFunc m_stackStencilFunc[64];
        /* 0xe314 */ uint32_t m_stackTopStencilFunc{0};
        /* 0xe318 */ StencilOp m_stackStencilFail[64];
        /* 0xe418 */ uint32_t m_stackTopStencilFail{0};
        /* 0xe41c */ StencilOp m_stackStencilZFail[64];
        /* 0xe51c */ uint32_t m_stackTopStencilZFail{0};
        /* 0xe520 */ StencilOp m_stackStencilPass[64];
        /* 0xe620 */ uint32_t m_stackTopStencilPass{0};
        /* 0xe624 */ bool m_stackStencil2SidedEnable[64];
        /* 0xe664 */ uint32_t m_stackTopStencil2SidedEnable{0};
        /* 0xe668 */ CmpFunc m_stackStencilCcwFunc[64];
        /* 0xe768 */ uint32_t m_stackTopStencilCcwFunc{0};
        /* 0xe76c */ StencilOp m_stackStencilCcwFail[64];
        /* 0xe86c */ uint32_t m_stackTopStencilCcwFail{0};
        /* 0xe870 */ StencilOp m_stackStencilCcwZFail[64];
        /* 0xe970 */ uint32_t m_stackTopStencilCcwZFail{0};
        /* 0xe974 */ StencilOp m_stackStencilCcwPass[64];
        /* 0xea74 */ uint32_t m_stackTopStencilCcwPass{0};
        /* 0xea78 */ bool m_stackMultiSample[64];
        /* 0xeab8 */ uint32_t m_stackTopMultiSample{0};
        /* 0xeabc */ uint32_t m_stackMultiSampleMask[64];
        /* 0xebbc */ uint32_t m_stackTopMultiSampleMask{0};
        /* 0xebc0 */ bool m_updateModelViewProj;
        /* 0xebc1 */ bool m_updateModelViewProjWithWorld;
        /* 0xebc2 */ bool m_updateModelMatrix;
        /* 0xebc4 */ VbHandle m_fsQuadVb;
        /* 0xebc8 */ IbHandle m_fsQuadIb;
        /* 0xebcc */ VbHandle m_vbXyz;
        /* 0xebd0 */ VbHandle m_vbXyzc;
        /* 0xebd4 */ VbHandle m_vbXyznc;
        /* 0xebd8 */ VbHandle m_vbXyzct1;
        /* 0xebdc */ VbHandle m_vbXyzwct1;
        /* 0xebe0 */ VbHandle m_vbXyznt1;
        /* 0xebe4 */ VbHandle m_vbXyznt2;
        /* 0xebe8 */ VbHandle m_vbXyznt3;
        /* 0xebec */ VbHandle m_vbXyznct2;
        /* 0xebf0 */ VbHandle m_vbXyznct1;
        /* 0xebf4 */ VbHandle m_vbXyznt1t;
        /* 0xebf8 */ VbHandle m_vbXyznct1t;
        /* 0xebfc */ D3DLIGHT9 m_lights[8];
        /* 0xef3c */ int32_t m_lightsEnabled[8];
        /* 0xef5c */ Colorf m_ambientLight;
        /* 0xef6c */ int32_t m_userClipPlaneEnabled{false};
        /* 0xef70 */ bool m_userClipPlanesUpdated[6];
        /* 0xef78 */ D3DXPLANE m_userClipPlanesWorld[6];
        /* 0xefd8 */ D3DXPLANE m_userClipPlanesProj[6];
        /* 0xf038 */ bool m_fastClipEnabled;
        /* 0xf03c */ D3DXPLANE m_fastClipPlane;
        /* 0xf04c */ hta::CMatrix m_fastClipPlaneProjMatrix;
        /* 0xf08c */ StateManager m_stateManager;
        /* 0xf098 */ ID3DXEffectPool* m_globalFxPool{nullptr};
        /* 0xf09c */ std::map<ShaderIdData, EffectImpl*> m_effects;
        /* 0xf0a8 */ std::map<ShaderIdData, HlslShaderImpl*> m_HlslShaders;
        /* 0xf0b4 */ std::map<hta::CStr, AsmShaderImpl*> m_AsmShaders;
        /* 0xf0c0 */ std::vector<MacroData> m_shadersMacros;
        /* 0xf0d0 */ std::vector<D3DXMACRO> m_d3dxMacros;
        /* 0xf0e0 */ std::list<IQuery*> m_queries;
        /* 0xf0ec */ bool m_featureSupported[27];
        /* 0xf108 */ IDirect3DTexture9* m_fsRt{nullptr};
        /* 0xf10c */ IDirect3DSurface9* m_fsRtSurf{nullptr};
        /* 0xf110 */ IDirect3DSurface9* m_fsRtZBuffer{nullptr};
        /* 0xf114 */ uint32_t m_fsRtWidth;
        /* 0xf118 */ uint32_t m_fsRtHeight;
        /* 0xf11c */ IDirect3DVertexBuffer9* m_fsRtVb{nullptr};
        /* 0xf120 */ IDirect3DIndexBuffer9* m_fsRtIb{nullptr};
        /* 0xf124 */ IHlslShader* m_fsRtVs{nullptr};
        /* 0xf128 */ IHlslShader* m_fsRtPs{nullptr};
        /* 0xf12c */ bool m_isNV30;
        /* 0xf130 */ hta::CMatrix m_viewMatrix;
        /* 0xf170 */ hta::CMatrix m_viewMatrixInv;
        /* 0xf1b0 */ hta::CVector m_viewOrigin;
        /* 0xf1bc */ bool m_viewMatrixWasSetThisFrame;
        /* 0xf1c0 */ ID3DXFont* m_pSysFont{nullptr};
        /* 0xf1c4 */ DXCursorInfo m_currentDXCursorInfo;
        /* 0xf1d4 */ int32_t m_activeStencilTarget{0};
        /* 0xf1d8 */ int32_t m_stencilLevel[2]{-1, -1};
        /* 0xf1e0 */ bool m_reloadAllTextures{false};
        /* 0xf1e4 */ uint32_t m_renderThreadId{0};
        /* 0xf1e8 */ bool m_bThreadSafeGuardEnabled{true};

        void _SetLastResult(HRESULT result);
        virtual int32_t DecRef();
        virtual int32_t IncRef();
        virtual void* QueryIface(const char*);
        void InitMatrices();
        void rstStacks();
        void logCaps();
        bool defineInstancingSupport();
        int32_t SetXFormMatrix(int32_t, const hta::CMatrix&);
        void InitVertexDeclarations();
        void DoneVertexDeclarations();
        void CreateCombinedVertexDeclaration(IDirect3DVertexDeclaration9*, IDirect3DVertexDeclaration9*);
        void ActuateStates(bool);
        void ActuateMatrices();
        void internalReset();
        void rstCaches();
        void rstTexPrepareFor();
        bool rstTexRestoreAfter();
        void rstIbPrepareFor();
        void rstIbRestoreAfter();
        void rstVbPrepareFor();
        void rstVbRestoreAfter();
        void GetVertexInfo(VertexType, uint32_t&, int32_t&, IDirect3DVertexDeclaration9*&);
        HRESULT setTexture(int32_t, IDirect3DBaseTexture9*);
        void setGamma(float, float, float);
        TexHandle readShader(const hta::CStr&, uint32_t);
        int32_t loadTexMaps(CTexture*, const hta::CStr&, uint32_t);
        void UpdateVBMemStats();
        void UpdateIBMemStats();
        void UpdateTexMemStats();
        int32_t FindDepthFormat(D3DFORMAT, D3DFORMAT*, int32_t);
        int32_t FindDepthFormat(D3DFORMAT&, D3DFORMAT, const D3DFORMAT*, const int32_t*, int32_t, int32_t);
        int32_t FindTexFormat(D3DFORMAT&, D3DFORMAT, const D3DFORMAT*, const int32_t*, int32_t, int32_t, int32_t);
        int32_t FindSurfaceFormat(D3DFORMAT*, int32_t);
        int32_t FindTextureFormat(D3DFORMAT, int32_t);
        virtual bool IsMultiSamplingSupported(int32_t) const override;
        bool IsMultiSamplingSupported(D3DFORMAT, D3DMULTISAMPLE_TYPE, uint32_t*) const;
        int32_t FindMultisampleType(D3DFORMAT, D3DFORMAT, int32_t, D3DPRESENT_PARAMETERS&);
        D3DFORMAT GetTexFormat(bool, bool);
        D3DCAPS9 GetCaps();
        D3DFORMAT GetTexFormatRt();
        D3DFORMAT GetTexFormatShadow();
        D3DFORMAT GetTexFormatDepth();
        D3DFORMAT GetSurfaceFormat();
        D3DFORMAT GetDepthStencilFormat();
        HRESULT setRenderState(D3DRENDERSTATETYPE, uint32_t);
        uint32_t getRenderState(D3DRENDERSTATETYPE);
        HRESULT setTextureStageState(uint32_t, D3DTEXTURESTAGESTATETYPE, uint32_t);
        HRESULT setTextureSamplerState(uint32_t, D3DSAMPLERSTATETYPE, uint32_t);
        HRESULT setStreamSourceFreq(uint32_t, uint32_t);
        HRESULT setFVF(uint32_t);
        HRESULT setVertexDeclaration(IDirect3DVertexDeclaration9*);
        HRESULT setStreamSource(int32_t, IDirect3DVertexBuffer9*, uint32_t);
        void setIndices(IDirect3DIndexBuffer9*, uint32_t);
        HRESULT setVertexShader(IDirect3DVertexShader9*);
        HRESULT setPixelShader(IDirect3DPixelShader9*);
        bool IsIbValid(const IbHandle&) const;
        bool IsMeshValid(const MeshHandle&) const;
        bool IsVbValid(const VbHandle&) const;
        bool IsTexValid(const TexHandle&) const;
        CDevice(const CDevice&);
        CDevice();
        virtual ~CDevice();
        IDirect3DDevice9* GetDevice();
        IDirect3D9* GetInterface();
        int32_t GetLastResult() const;
        IDirect3DCubeTexture9* GetNormalCubemap();
        virtual bool Create(void(*)(const hta::CStr&), hta::m3d::Kernel*) override;
        virtual bool CreateDevice() override;
        virtual bool SwitchDisplayModes(HWND, int32_t, int32_t, bool) override;
        virtual bool Reset() override;
        int32_t SwitchDisplayModes0(HWND, int32_t, int32_t, int32_t);
        virtual void PushAlphaTest(int32_t);
        virtual void DuplicateAlphaTest();
        virtual void PopAlphaTest();
        virtual void SetAlphaTest(int32_t, bool);
        virtual void SetAlphaTest(int32_t) override;
        virtual void PushBlend(BlendMode) override;
        virtual void DuplicateBlend() override;
        virtual void PopBlend() override;
        virtual void SetBlend(BlendMode, bool) override;
        virtual void PushZbState(ZbState) override;
        virtual void DuplicateZbState() override;
        virtual void PopZbState() override;
        virtual void SetZbState(ZbState, bool) override;
        virtual void PushCull(Cull) override;
        virtual void DuplicateCull() override;
        virtual void PopCull() override;
        virtual void SetCull(Cull, bool) override;
        virtual void PushZFunc(CmpFunc) override;
        virtual void DuplicateZFunc() override;
        virtual void PopZFunc() override;
        virtual void SetZFunc(CmpFunc, bool) override;
        virtual void PushLighting(bool) override;
        virtual void DuplicateLighting() override;
        virtual void PopLighting() override;
        virtual void SetLighting(bool, bool) override;
        virtual void PushAmbient(uint32_t) override;
        virtual void DuplicateAmbient() override;
        virtual void PopAmbient() override;
        virtual void SetAmbient(uint32_t, bool) override;
        virtual void PushFog(bool) override;
        virtual void DuplicateFog() override;
        virtual void PopFog() override;
        virtual void SetFog(bool, bool) override;
        virtual void PushFogColor(uint32_t) override;
        virtual void DuplicateFogColor() override;
        virtual void PopFogColor() override;
        virtual void SetFogColor(uint32_t, bool) override;
        virtual void PushFogMode(FogMode) override;
        virtual void DuplicateFogMode() override;
        virtual void PopFogMode() override;
        virtual void SetFogMode(FogMode, bool) override;
        virtual void PushFogStart(float) override;
        virtual void DuplicateFogStart() override;
        virtual void PopFogStart() override;
        virtual void SetFogStart(float, bool) override;
        virtual void PushFogEnd(float) override;
        virtual void DuplicateFogEnd() override;
        virtual void PopFogEnd() override;
        virtual void SetFogEnd(float, bool) override;
        virtual void PushFillMode(FillMode) override;
        virtual void DuplicateFillMode() override;
        virtual void PopFillMode() override;
        virtual void SetFillMode(FillMode, bool) override;
        virtual void PushZBias(float) override;
        virtual void DuplicateZBias() override;
        virtual void PopZBias() override;
        virtual void SetZBias(float, bool) override;
        virtual void PushZBiasSlopeScale(float) override;
        virtual void DuplicateZBiasSlopeScale() override;
        virtual void PopZBiasSlopeScale() override;
        virtual void SetZBiasSlopeScale(float, bool) override;
        virtual void PushShadeMode(ShadeMode) override;
        virtual void DuplicateShadeMode() override;
        virtual void PopShadeMode() override;
        virtual void SetShadeMode(ShadeMode, bool) override;
        virtual void PushPointSpriteEnable(bool) override;
        virtual void DuplicatePointSpriteEnable() override;
        virtual void PopPointSpriteEnable() override;
        virtual void SetPointSpriteEnable(bool, bool) override;
        virtual void PushPointScaleEnable(bool) override;
        virtual void DuplicatePointScaleEnable() override;
        virtual void PopPointScaleEnable() override;
        virtual void SetPointScaleEnable(bool, bool) override;
        virtual void PushPointSizeMin(float) override;
        virtual void DuplicatePointSizeMin() override;
        virtual void PopPointSizeMin() override;
        virtual void SetPointSizeMin(float, bool) override;
        virtual void PushPointSizeMax(float) override;
        virtual void DuplicatePointSizeMax() override;
        virtual void PopPointSizeMax() override;
        virtual void SetPointSizeMax(float, bool) override;
        virtual void PushPointSize(float) override;
        virtual void DuplicatePointSize() override;
        virtual void PopPointSize() override;
        virtual void SetPointSize(float, bool) override;
        virtual void PushPointScaleA(float) override;
        virtual void DuplicatePointScaleA() override;
        virtual void PopPointScaleA() override;
        virtual void SetPointScaleA(float, bool) override;
        virtual void PushPointScaleB(float) override;
        virtual void DuplicatePointScaleB() override;
        virtual void PopPointScaleB() override;
        virtual void SetPointScaleB(float, bool) override;
        virtual void PushPointScaleC(float) override;
        virtual void DuplicatePointScaleC() override;
        virtual void PopPointScaleC() override;
        virtual void SetPointScaleC(float, bool) override;
        virtual void PushTFactor(uint32_t) override;
        virtual void DuplicateTFactor() override;
        virtual void PopTFactor() override;
        virtual void SetTFactor(uint32_t, bool) override;
        virtual void PushLocalViewer(bool) override;
        virtual void DuplicateLocalViewer() override;
        virtual void PopLocalViewer() override;
        virtual void SetLocalViewer(bool, bool) override;
        virtual void PushSpecularLighting(bool) override;
        virtual void DuplicateSpecularLighting() override;
        virtual void PopSpecularLighting() override;
        virtual void SetSpecularLighting(bool, bool) override;
        virtual void PushColorWriteMask(uint32_t) override;
        virtual void DuplicateColorWriteMask() override;
        virtual void PopColorWriteMask() override;
        virtual void SetColorWriteMask(uint32_t, bool) override;
        virtual void PushDithering(bool);
        virtual void DuplicateDithering();
        virtual void PopDithering();
        virtual void SetDithering(bool, bool);
        virtual void PushNPatchLevel(float) override;
        virtual void DuplicateNPatchLevel() override;
        virtual void PopNPatchLevel() override;
        virtual void SetNPatchLevel(float, bool) override;
        virtual void PushStencilState(bool) override;
        virtual void DuplicateStencilState() override;
        virtual void PopStencilState() override;
        virtual void SetStencilState(bool, bool) override;
        virtual void PushStencilMask(uint32_t) override;
        virtual void DuplicateStencilMask() override;
        virtual void PopStencilMask() override;
        virtual void SetStencilMask(uint32_t, bool) override;
        virtual void PushStencilRef(uint32_t) override;
        virtual void DuplicateStencilRef() override;
        virtual void PopStencilRef() override;
        virtual void SetStencilRef(uint32_t, bool) override;
        virtual void PushStencilWriteMask(uint32_t) override;
        virtual void DuplicateStencilWriteMask() override;
        virtual void PopStencilWriteMask() override;
        virtual void SetStencilWriteMask(uint32_t, bool) override;
        virtual void PushStencilFunc(CmpFunc) override;
        virtual void DuplicateStencilFunc() override;
        virtual void PopStencilFunc() override;
        virtual void SetStencilFunc(CmpFunc, bool) override;
        virtual void PushStencilFail(StencilOp) override;
        virtual void DuplicateStencilFail() override;
        virtual void PopStencilFail() override;
        virtual void SetStencilFail(StencilOp, bool) override;
        virtual void PushStencilZFail(StencilOp) override;
        virtual void DuplicateStencilZFail() override;
        virtual void PopStencilZFail() override;
        virtual void SetStencilZFail(StencilOp, bool) override;
        virtual void PushStencilPass(StencilOp) override;
        virtual void DuplicateStencilPass() override;
        virtual void PopStencilPass() override;
        virtual void SetStencilPass(StencilOp, bool) override;
        virtual void PushStencil2SidedEnable(bool) override;
        virtual void DuplicateStencil2SidedEnable() override;
        virtual void PopStencil2SidedEnable() override;
        virtual void SetStencil2SidedEnable(bool, bool) override;
        virtual void PushStencilCcwFunc(CmpFunc) override;
        virtual void DuplicateStencilCcwFunc() override;
        virtual void PopStencilCcwFunc() override;
        virtual void SetStencilCcwFunc(CmpFunc, bool) override;
        virtual void PushStencilCcwFail(StencilOp) override;
        virtual void DuplicateStencilCcwFail() override;
        virtual void PopStencilCcwFail() override;
        virtual void SetStencilCcwFail(StencilOp, bool) override;
        virtual void PushStencilCcwZFail(StencilOp) override;
        virtual void DuplicateStencilCcwZFail() override;
        virtual void PopStencilCcwZFail() override;
        virtual void SetStencilCcwZFail(StencilOp, bool) override;
        virtual void PushStencilCcwPass(StencilOp) override;
        virtual void DuplicateStencilCcwPass() override;
        virtual void PopStencilCcwPass() override;
        virtual void SetStencilCcwPass(StencilOp, bool) override;
        virtual void PushMultiSample(int32_t) override;
        virtual void DuplicateMultiSample() override;
        virtual void PopMultiSample() override;
        virtual void SetMultiSample(int32_t, bool) override;
        virtual void PushMultiSampleMask(uint32_t) override;
        virtual void DuplicateMultiSampleMask() override;
        virtual void PopMultiSampleMask() override;
        virtual void SetMultiSampleMask(uint32_t, bool) override;
        virtual void SetViewMatrix(const hta::CMatrix&);
        virtual const hta::CMatrix& GetViewMatrix() const override;
        const hta::CMatrix& GetInvViewMatrix() const;
        virtual const hta::CVector& GetViewOrigin() const override;
        virtual void MatDuplicate() override;
        virtual void MatPush(const hta::CMatrix&) override;
        virtual void MatPop(bool) override;
        virtual void MatMul(const hta::CMatrix&) override;
        virtual void MatMulR(const hta::CMatrix&) override;
        virtual const hta::CMatrix& MatGet() const;
        virtual const hta::CMatrix& MatGetInv() override;
        virtual void MatSet(const hta::CMatrix&) override;
        virtual void MatGetBasis(hta::CVector&, hta::CVector&, hta::CVector&) const override;
        virtual hta::CVector MatGetOrgInv() override;
        virtual hta::CVector MatGetOrg() const override;
        void MatGetYPR(float&, float&, float&);
        virtual void MatSetWorld(const hta::CMatrix&) override;
        virtual const hta::CMatrix& MatGetWorld() const override;
        virtual void MatDuplicateWorld() override;
        virtual void MatPopWorld() override;
        virtual void MatSetProj(const hta::CMatrix&) override;
        virtual const hta::CMatrix& MatGetProj() const override;
        virtual void MatDuplicateProj() override;
        virtual void MatPopProj() override;
        void ActuateProjectionMatrix();
        virtual const hta::CMatrix& GetModelMatrix();
        virtual const hta::CMatrix& GetModelViewProjMatrix() override;
        virtual hta::CVector Unproject(const hta::CVector2&) override;
        virtual hta::CVector Project(const hta::CVector&) override;
        virtual hta::CVector ProjectWorldAbs(const hta::CVector&) override;
        virtual void TgEnableSetLinearSt(int32_t, float, float, float, float, float, bool, float, float, float, float) override;
        virtual void TgEnableSetMatrixSt(int32_t, const hta::CMatrix&, bool) override;
        virtual void TgEnableSetMatrixStr(int32_t, const hta::CMatrix&, bool) override;
        void TgEnableSetMatrixStrReflection(int32_t, const hta::CMatrix&, bool);
        virtual void TgSetTransformMode(int32_t, TgMode) override;
        virtual void TgSetTcSource(int32_t, TcSource, int32_t) override;
        virtual void TgDisable(int32_t) override;
        virtual void SetTextureMatrix(int32_t, const hta::CMatrix&) override;
        virtual void Set2x2BumpMatrix(int32_t, float, float, float, float) override;
        int32_t RenderToTexStart(const TexHandle&, bool, const TexHandle&);
        virtual int32_t RenderToTexStart(const TexHandle&, bool) override;
        virtual void RenderToTexFinish() override;
        virtual void CopyRenderTargetToTexture(const TexHandle&) override;
        virtual int32_t SetActiveState(int32_t) override;
        virtual int32_t CanRender() override;
        virtual int32_t BeginScene() override;
        virtual int32_t InScene() override;
        virtual int32_t EndScene() override;
        virtual int32_t PresentScene() override;
        virtual void ClearViewport(ClearFlags, uint32_t) override;
        virtual Viewport GetViewport() const override;
        virtual int32_t SetViewport(const Viewport&) override;
        virtual void RegisterResetCallback(hta::m3d::IDeviceResetCallback*) override;
        virtual void UnregisterResetCallback(hta::m3d::IDeviceResetCallback*) override;
        virtual void SetGamma(float, float, float) override;
        virtual bool IsFeatureSupported(DeviceFeature) const override;
        virtual void SetStageState(int32_t, BlendMode, TextureState) override;
        virtual void SetStreamFrequency(int32_t, StreamDataType, uint32_t);
        virtual float GetMaxPointSize() override;
        virtual float GetMaxNPatchTessellationLevel() override;
        virtual int32_t GetMaxVertexShaderConst() override;
        CImage* addTexMap(const hta::CStr&, uint32_t);
        CImage* addTexMap_(uint32_t, const hta::CStr&, uint32_t);
        virtual TexHandle AddTexture(const hta::CStr&, uint32_t) override;
        virtual TexHandle AddDynamicTexture(const char*, int32_t, int32_t, uint32_t) override;
        virtual TexHandle GetFullFrameFrameBufferTexture() override;
        virtual bool ReportTexturesInfo(const char*) override;
        virtual TexHandle AddRenderTargetTexture(const char*, int32_t, int32_t) override;
        virtual TexHandle GetBufferedTargetTexture(int32_t);
        bool IsDynamic(const CTexture&) const;
        virtual int32_t ReloadTextures();
        virtual int32_t SetTexture(int32_t, const TexHandle&, double);
        virtual void SetWhiteTexture(int32_t);
        virtual void SetBlackTexture(int32_t);
        virtual void SetErrorTexture(int32_t);
        virtual void DisableTextureStages(int32_t);
        virtual void SetTexAnimStart(const TexHandle&);
        virtual int32_t ReferenceTexture(const TexHandle&);
        virtual int32_t ReleaseTexture(TexHandle&);
        virtual int32_t GetTextureName(const TexHandle&, hta::CStr&);
        virtual void SetTextureParameter(const TexHandle&, TexParam, uint32_t);
        virtual int32_t UploadTexImage(const TexHandle&, uint32_t, uint32_t, unsigned char*, TexDynFormat, int32_t);
        virtual void* LockTexture(const TexHandle&, TexDynFormat, int32_t&, int32_t);
        virtual void UnlockTexture(const TexHandle&);
        virtual int32_t DownloadPureTexImageRgba8888(void*, const TexHandle&) override;
        virtual int32_t DownloadSizedTexImageRgba8888(void*, const TexHandle&, int32_t, int32_t) override;
        virtual void GetDims(const TexHandle&, int32_t&, int32_t&) const;
        virtual void TexCopy(const TexHandle&, const TexHandle&, bool);
        virtual void TexCopy(const TexHandle&, const TexHandle&) override;
        virtual void CreateMips(const TexHandle&);
        virtual void RepaintAllTexturesMips();
        void initFullScreenQuad();
        void doneFullScreenQuadStuff();
        void DrawFullScreenQuad(bool);
        virtual void DrawFullScreenQuadEffect(IEffect*);
        virtual void DrawFullScreenQuad() override;
        virtual IbHandle AddIb(int32_t, bool);
        IbHandle AddIbStreaming(int32_t);
        virtual void SetPoolIndices(const IbPoolField&, uint32_t) override;
        virtual void SetHandleIndices(const IbHandle&, int32_t) override;
        virtual void* LockIb(const IbHandle&, int32_t, int32_t, uint32_t);
        virtual void* LockIbStreaming(const IbHandle&, int32_t, int32_t&, int32_t*);
        virtual void UnlockIb(const IbHandle&);
        virtual int32_t ReferenceIb(const IbHandle&);
        virtual int32_t ReleaseIb(IbHandle&);
        virtual IbPoolField AddIbPoolField(uint32_t);
        virtual void ReleaseIbPoolField(IbPoolField&);
        virtual void* LockIbPoolField(const IbPoolField&);
        virtual void UnlockIbPoolField(const IbPoolField&);
        virtual bool ReportVbsInfo(const char*);
        virtual VbHandle AddVb(VertexType, int32_t, const hta::CStr&, uint32_t) override;
        VbHandle AddVbStreaming(VertexType, int32_t, uint32_t);
        virtual void SetPoolToStream0(const VbPoolField&);
        virtual void SetHandleToStream0(const VbHandle&);
        virtual void SetPoolToStream(int32_t, const VbPoolField&);
        virtual void SetHandleToStream(int32_t, const VbHandle&);
        virtual void SetToStreams01(const VbHandle&, const VbHandle&);
        virtual void* LockVb(const VbHandle&, int32_t, int32_t, uint32_t);
        virtual void* LockVbStreaming(const VbHandle&, int32_t, int32_t&, int32_t*);
        virtual void UnlockVb(const VbHandle&);
        virtual int32_t ReferenceVb(const VbHandle&);
        virtual int32_t ReleaseVb(VbHandle&);
        virtual bool ReportIbsInfo(const char*);
        virtual VbPoolField AddVbPoolField(VertexType, uint32_t);
        virtual void ReleaseVbPoolField(VbPoolField&);
        virtual void* LockVbPoolField(const VbPoolField&);
        virtual void UnlockVbPoolField(const VbPoolField&);
        virtual VbHandle GetVbStreaming(VertexType);
        virtual int32_t DrawIndexedPrimitive(PrimType, uint32_t, uint32_t, uint32_t, uint32_t);
        virtual int32_t DrawPrimitive(PrimType, uint32_t, uint32_t);
        virtual int32_t GetMaxLights() const;
        virtual void LightEnable(int32_t, int32_t);
        virtual int32_t LightSet(int32_t, const _D3DLIGHT9&);
        virtual void LightSet(int32_t, const LightSource&);
        virtual void MaterialSet(const Material&);
        virtual void RelToAbs(float&, float&) const;
        virtual void AbsToRel(float&, float&) const;
        virtual int32_t AddTextureFromBackBufferHandle(TexHandle);
        virtual TexHandle AddTextureFromBackBuffer(int32_t, int32_t);
        virtual void ScreenShot(const char*, int32_t, int32_t);
        virtual int32_t SaveTextureToTgaFile(TexHandle, const char*);
        virtual int32_t SaveTextureToFile(TexHandle, const char*, ImageFileFormats);
        virtual char* GetCurBppStr(int32_t*) const;
        virtual char* GetLastErrorStr() const;
        void ForceRecalcClipPlanes();
        virtual uint32_t GetMaxClipPlanes() const;
        virtual void SetClipPlane(int32_t, const hta::CPlane&);
        virtual void EnableClipPlane(int32_t, bool);
        void ActuateClipPlanes(bool);
        virtual int32_t GetMaxAnisotropy();
        void EnableFastClipPlane(bool);
        void SetFastClipPlane(const hta::CPlane&);
        virtual void ResetStats();
        virtual void GetStats(RenderStats&) const;
        virtual void GetDeviceMemStats(DeviceMemStats&);
        virtual void ShowStats() const;
        MeshHandle AddMesh(VertexType, void*, int32_t, uint16_t*, int32_t, SubmeshInfo*, int32_t, int32_t*);
        int32_t ReleaseMesh(MeshHandle&);
        void RenderMesh(const MeshHandle&, float);
        void StartRenderMeshes();
        void FinishRenderMeshes();
        virtual int32_t
        OptimizeGeometryToSingleStrip(VertexType, void*, int32_t, uint16_t*, int32_t, int32_t, void**, int32_t*, int32_t**, uint16_t**, int32_t*);
        virtual int32_t
        OptimizeGeometryToTriList(VertexType, void*, int32_t, uint16_t*, int32_t, int32_t, void**, int32_t*, int32_t**, uint16_t**, int32_t*);
        virtual void* GetInternalData();
        virtual void SetVsFloatConst(uint32_t, const float*, uint32_t);
        virtual void SetVsIntConst(uint32_t, const int32_t*, uint32_t);
        virtual void SetVsBoolConst(uint32_t, const int32_t*, uint32_t);
        virtual void SetPsFloatConst(uint32_t, const float*, uint32_t);
        virtual void SetPsIntConst(uint32_t, const int32_t*, uint32_t);
        virtual void SetPsBoolConst(uint32_t, const int32_t*, uint32_t);
        virtual IAsmShader* NewAsmShader(const char*, IAsmShader::Type);
        virtual IHlslShader*
        NewHlslShader(const char*, const char*, IHlslShader::Profile, const std::vector<enum CompileParam, std::allocator<enum CompileParam>>&);
        virtual IHlslShader* NewHlslShader(const char*, const char*, IHlslShader::Profile) override;
        virtual IEffect* NewEffect(const char*, bool, const std::vector<enum CompileParam, std::allocator<enum CompileParam>>&);
        virtual IEffect* NewEffect(const char*, bool) override;
        virtual const char* EffectParameterToString(IEffect::Parameter);
        virtual IEffect::Parameter StringToEffectParameter(const char*);
        virtual const char* CompileParamToString(CompileParam);
        virtual CompileParam StringToCompileParam(const char*);
        virtual bool ReloadShaders();
        virtual void AddChangeShaderMacro(const ShaderMacro&);
        virtual void DeleteShaderMacro(const hta::CStr&);
        virtual int32_t DrawIndexedPrimitiveEffect(PrimType, IEffect*, uint32_t, uint32_t, uint32_t, uint32_t);
        virtual int32_t DrawPrimitiveEffect(PrimType, IEffect*, uint32_t, uint32_t);
        virtual int32_t DrawIndexedPrimitiveShader(PrimType, uint32_t, uint32_t, uint32_t, uint32_t);
        virtual int32_t DrawPrimitiveShader(PrimType, uint32_t, uint32_t);
        virtual IQuery* NewQuery(IQuery::Type);
        void OnEffectDestructor(EffectImpl*);
        void OnHlslShaderDestructor(HlslShaderImpl*);
        void OnAsmShaderDestructor(AsmShaderImpl*);
        void rstShadersPrepareFor();
        void rstShadersRestoreAfter();
        void ReleaseAsmShaders();
        void ReleaseHlslShaders();
        void ReleaseEffects();
        void createD3DXMacros();
        void addMacro(const hta::CStr&, const hta::CStr&, bool);
        void loadShadersMacros();
        void saveShadersMacros();
        void ReleaseQueries();
        void OnQueryDestructor(IQuery*);
        void rstQueryPrepareFor();
        void rstQueryRestoreAfter();
        void CreateFsRt();
        void ReleaseFsRt();
        void StartRenderingToFsRt();
        void FinishRenderingToFsRt();
        void DrawFsRt();
        virtual int32_t SetupDXCursor(const TexHandle&, int32_t, int32_t, int32_t);
        virtual void MoveDXCursor(int32_t, int32_t);
        virtual void ShowDXCursor(bool);
        virtual int32_t UpdateDXCursorFrame();
        virtual bool IsHardWareCursorAvailableForTexture(const TexHandle&) const;
        virtual bool IsNV3x() const;
        virtual void SingleLayerStencilStart();
        virtual void SingleLayerStencilContinue();
        virtual void SingleLayerStencilFinish();
        virtual int32_t SetupDXCursorForce(const TexHandle&, int32_t, int32_t, int32_t);
        int32_t GetTextureCurrentFrame(const TexHandle&, double);
        int32_t GetTextureCurrentFrame(CTexture*, double);
        int32_t SetupAllFeaturesSupport(const hta::CStr&);
        void rstStencilLevels();
        virtual void ResetTextureStates();
        virtual void SetRenderThreadId(uint32_t);
        virtual void EnableThreadSafeQuard(bool);

        static CDevice* Instance(void);
    };
};