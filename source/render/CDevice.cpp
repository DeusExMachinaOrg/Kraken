#define LOGGER "render"

#include "ext/logger.hpp"

#include "hta/CPlane.hpp"
#include "hta/m3d/EngineConfig.hpp"
#include "hta/m3d/Kernel.hpp"
#include "hta/m3d/fs/FileServer.hpp"
#include "hta/m3d/rend/Vertices.hpp"
#include "render/CDevice.hpp"

#include <algorithm>
#include <exception>
#include <set>
#include <unordered_map>

#define _PROTECT_UNDERFLOW(field)                                                                                                          \
    if (field > 63) { /* Wrapped from 0 to 0xFFFFFFFF */                                                                                   \
        LOG_WARNING(#field " stack UNDERFLOW! Setting to 1");                                                                              \
        field = 1; /* Step back into valid range */                                                                                        \
    }

#define _PROTECT_OVERFLOW(field)                                                                                                           \
    if (field > 63) { /* Went from 63 to 64+ */                                                                                            \
        LOG_WARNING(#field " stack OVERFLOW! Setting to 62");                                                                              \
        field = 62; /* Step back into valid range */                                                                                       \
    }

namespace kraken::render {
    static CDevice* G_DEVICE = nullptr;

    Query::Query(IQuery::Type type) {
        m_refCount      = 0;
        m_type          = type;
        m_query         = nullptr;
        m_state         = QUERY_ERROR;
        m_retVal.m_type = QueryReturnValue::NotValid;
    };

    int32_t Query::CreateQuery() {
        m_state = QUERY_SIGNALED;

        return SUCCEEDED(CDevice::Instance()->m_pd3dDevice->CreateQuery(static_cast<D3DQUERYTYPE>(m_type), &m_query));
    };

    void Query::ReleaseQuery() {
        if (m_query) {
            m_query->Release();
            m_query = nullptr;
        }
    };

    IQuery::Type Query::GetType() const {
        return m_type;
    };

    void Query::Begin() {
        switch (m_type) {
        case QUERY_VCACHE:
        case QUERY_EVENT:
        case QUERY_TIMESTAMP:
        case QUERY_TIMESTAMPFREQ:
            // These query types don't support Begin
            return;

        default:
            m_query->Issue(D3DISSUE_BEGIN);
            break;
        }
    };

    void Query::End() {
        m_query->Issue(D3DISSUE_END);
    };

    IQuery::State Query::GetState() {
        if (m_state == QUERY_NOT_SUPPORT)
            return QUERY_NOT_SUPPORT;

        HRESULT hr = m_query->GetData(nullptr, 0, 0);

        if (SUCCEEDED(hr)) {
            m_state = QUERY_SIGNALED;
        } else if (hr == S_FALSE) {
            m_state = QUERY_ISSUED;
        } else {
            m_state = QUERY_ERROR;
        }

        return m_state;
    };

    bool Query::IsValid() const {
        return m_query != nullptr;
    };

    const QueryReturnValue& Query::GetData() {
        if (GetState() == QUERY_ERROR)
            return m_retVal;

        switch (m_type) {
        case QUERY_VCACHE:
            m_retVal.m_type = QueryReturnValue::VCache;
            while (m_query->GetData(&m_retVal.data, sizeof(D3DDEVINFO_VCACHE), D3DGETDATA_FLUSH) == S_FALSE)
                ; // Spin until data ready
            break;

        case QUERY_EVENT:
            m_retVal.m_type = QueryReturnValue::Bool;
            while (m_query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE)
                ; // Spin until event signaled
            break;

        case QUERY_OCCLUSION:
            m_retVal.m_type = QueryReturnValue::Dword;
            while (m_query->GetData(&m_retVal.data, sizeof(DWORD), D3DGETDATA_FLUSH) == S_FALSE)
                ; // Spin until data ready
            break;

        case QUERY_TIMESTAMP:
            m_retVal.m_type = QueryReturnValue::Uint64;
            while (m_query->GetData(&m_retVal.data, sizeof(UINT64), D3DGETDATA_FLUSH) == S_FALSE)
                ; // Spin until data ready
            break;

        case QUERY_TIMESTAMPDISJOINT:
            m_retVal.m_type = QueryReturnValue::Bool;
            while (m_query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE)
                ; // Spin until data ready
            break;

        case QUERY_TIMESTAMPFREQ:
            m_retVal.m_type = QueryReturnValue::Uint64;
            while (m_query->GetData(&m_retVal.data, sizeof(UINT64), D3DGETDATA_FLUSH) == S_FALSE)
                ; // Spin until data ready
            break;

        case QUERY_PIPELINETIMINGS:
            m_retVal.m_type = QueryReturnValue::PipelineTimings;
            while (m_query->GetData(&m_retVal.data, sizeof(D3DDEVINFO_D3D9PIPELINETIMINGS), D3DGETDATA_FLUSH) == S_FALSE)
                ; // Spin until data ready
            break;

        case QUERY_INTERFACETIMINGS:
            m_retVal.m_type = QueryReturnValue::InterfaceTimings;
            while (m_query->GetData(&m_retVal.data, sizeof(D3DDEVINFO_D3D9INTERFACETIMINGS), D3DGETDATA_FLUSH) == S_FALSE)
                ; // Spin until data ready
            break;

        case QUERY_VERTEXTIMINGS:
            m_retVal.m_type = QueryReturnValue::StageTimings;
            while (m_query->GetData(&m_retVal.data, sizeof(D3DDEVINFO_D3D9STAGETIMINGS), D3DGETDATA_FLUSH) == S_FALSE)
                ; // Spin until data ready
            break;

        case QUERY_BANDWIDTHTIMINGS:
            m_retVal.m_type = QueryReturnValue::BandWidthTimings;
            while (m_query->GetData(&m_retVal.data, sizeof(D3DDEVINFO_D3D9BANDWIDTHTIMINGS), D3DGETDATA_FLUSH) == S_FALSE)
                ; // Spin until data ready
            break;

        case QUERY_CACHEUTILIZATION:
            m_retVal.m_type = QueryReturnValue::CacheUtilization;
            while (m_query->GetData(&m_retVal.data, sizeof(D3DDEVINFO_D3D9CACHEUTILIZATION), D3DGETDATA_FLUSH) == S_FALSE)
                ; // Spin until data ready
            break;

        default:
            break;
        }

        return m_retVal;
    };

    Query::~Query() {
        if (m_query) {
            m_query->Release();
            m_query = nullptr;
        }

        CDevice::Instance()->OnQueryDestructor(this);
    };

    StateManager::StateManager() {
        this->m_dev   = 0;
        this->m_nRefs = 0;
    };

    void StateManager::SetDevice(CDevice* owner) {
        this->m_dev = CDevice::Instance();
    };

    HRESULT StateManager::QueryInterface(const GUID& iid, void** ppv) {
        *ppv = 0;
        return 0x8000FFFF;
    };

    ULONG StateManager::AddRef() {
        return ++this->m_nRefs;
    };

    ULONG StateManager::Release() {
        return --this->m_nRefs;
    };

    HRESULT StateManager::SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* pMatrix) {
        return CDevice::Instance()->SetXFormMatrix(state, *(hta::CMatrix*)pMatrix);
    };

    HRESULT StateManager::SetMaterial(const D3DMATERIAL9* pMaterial) {
        return CDevice::Instance()->m_pd3dDevice->SetMaterial(pMaterial);
    };

    HRESULT StateManager::SetLight(DWORD Index, const D3DLIGHT9* pLight) {
        return CDevice::Instance()->LightSet(Index, *pLight);
    };

    HRESULT StateManager::LightEnable(DWORD Index, BOOL Enable) {
        CDevice::Instance()->LightEnable(Index, Enable);
        return 0;
    };

    HRESULT StateManager::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
        return CDevice::Instance()->setRenderState(State, Value);
    };

    HRESULT StateManager::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) {
        return CDevice::Instance()->m_pd3dDevice->SetTexture(Stage, pTexture);
        return S_OK;
    };

    HRESULT StateManager::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
        return CDevice::Instance()->setTextureStageState(Stage, Type, Value);
    };

    HRESULT StateManager::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
        return CDevice::Instance()->setTextureSamplerState(Sampler, Type, Value);
    };

    HRESULT StateManager::SetNPatchMode(float) {
        return 0;
    };

    HRESULT StateManager::SetFVF(DWORD FVF) {
        return CDevice::Instance()->setFVF(FVF);
    };

    HRESULT StateManager::SetVertexShader(IDirect3DVertexShader9* pShader) {
        return CDevice::Instance()->setVertexShader(pShader);
    };

    HRESULT StateManager::SetVertexShaderConstantF(uint32_t RegisterIndex, const float* pConstantData, uint32_t RegisterCount) {
        CDevice::Instance()->SetVsFloatConst(RegisterIndex, pConstantData, RegisterCount);
        return S_OK;
    };

    HRESULT StateManager::SetVertexShaderConstantI(uint32_t RegisterIndex, const int32_t* pConstantData, uint32_t RegisterCount) {
        CDevice::Instance()->SetVsIntConst(RegisterIndex, pConstantData, RegisterCount);
        return S_OK;
    }

    HRESULT StateManager::SetVertexShaderConstantB(uint32_t RegisterIndex, const int32_t* pConstantData, uint32_t RegisterCount) {
        CDevice::Instance()->SetVsBoolConst(RegisterIndex, pConstantData, RegisterCount);
        return S_OK;
    };

    HRESULT StateManager::SetPixelShader(IDirect3DPixelShader9* pShader) {
        return CDevice::Instance()->setPixelShader(pShader);
    };

    HRESULT StateManager::SetPixelShaderConstantF(uint32_t RegisterIndex, const float* pConstantData, uint32_t RegisterCount) {
        CDevice::Instance()->SetPsFloatConst(RegisterIndex, pConstantData, RegisterCount);
        return S_OK;
    };

    HRESULT StateManager::SetPixelShaderConstantI(uint32_t RegisterIndex, const int32_t* pConstantData, uint32_t RegisterCount) {
        CDevice::Instance()->SetPsIntConst(RegisterIndex, pConstantData, RegisterCount);
        return S_OK;
    };

    HRESULT StateManager::SetPixelShaderConstantB(uint32_t RegisterIndex, const int32_t* pConstantData, uint32_t RegisterCount) {
        CDevice::Instance()->SetPsBoolConst(RegisterIndex, pConstantData, RegisterCount);
        return S_OK;
    };

    nShaderArg::nShaderArg() {
        this->m_type = Void;
        this->i      = 0;
    };

    nShaderArg::~nShaderArg() {};

    bool nShaderArg::operator==(const nShaderArg& other) const {
        if (this->m_type != other.m_type)
            return false;
        switch (this->m_type) {
        case Void:
            return true;
        case Bool:
            return this->b == other.b;
        case Int:
            return this->i == other.i;
        case Float:
            return this->f == other.f;
        case Float4:
            return this->f4.x == other.f4.x && this->f4.y == other.f4.y && this->f4.z == other.f4.z && this->f4.w == other.f4.w;
        case Matrix44:
            return this->m[0][0] == other.m[0][0] && this->m[0][1] == other.m[0][1] && this->m[0][2] == other.m[0][2] &&
                   this->m[0][3] == other.m[0][3] && this->m[1][0] == other.m[1][0] && this->m[1][1] == other.m[1][1] &&
                   this->m[1][2] == other.m[1][2] && this->m[1][3] == other.m[1][3] && this->m[2][0] == other.m[2][0] &&
                   this->m[2][1] == other.m[2][1] && this->m[2][2] == other.m[2][2] && this->m[2][3] == other.m[2][3] &&
                   this->m[3][0] == other.m[3][0] && this->m[3][1] == other.m[3][1] && this->m[3][2] == other.m[3][2] &&
                   this->m[3][3] == other.m[3][3];
        case Texture:
            return this->tex == other.tex;
        }
        return false;
    };

    void nShaderArg::SetType(Type t) {
        this->m_type = t;
    };

    nShaderArg::Type nShaderArg::GetType() const {
        return this->m_type;
    };

    void nShaderArg::SetBool(bool v) {
        this->m_type = Bool;
        this->b      = v;
    };

    bool nShaderArg::GetBool() const {
        return this->b;
    };

    void nShaderArg::SetInt(int32_t v) {
        this->m_type = Int;
        this->i      = v;
    };

    int32_t nShaderArg::GetInt() const {
        return this->i;
    };

    void nShaderArg::SetFloat(float v) {
        this->m_type = Float;
        this->f      = v;
    };

    float nShaderArg::GetFloat() const {
        return this->f;
    };

    void nShaderArg::SetFloat4(const hta::nFloat4& v) {
        this->m_type = Float4;
        this->f4     = v;
    };
    const hta::nFloat4& nShaderArg::GetFloat4() const {
        return this->f4;
    };

    void nShaderArg::SetMatrix44(const hta::CMatrix* v) {
        this->m_type = Matrix44;
        memcpy(&this->m, v, sizeof(this->m));
    }

    const hta::CMatrix* nShaderArg::GetMatrix44() const {
        return (hta::CMatrix*)(this->m);
    };

    void nShaderArg::SetTexture(TexHandle* v) {
        this->m_type = Texture;
        this->tex    = *v;
    };

    TexHandle* nShaderArg::GetTexture() {
        return &this->tex;
    };

    nShaderParams::nShaderParams() {
        std::fill_n(this->m_valid, this->SIZE, false);
    };

    nShaderParams::~nShaderParams() {};

    bool nShaderParams::IsParameterValid(IEffect::Parameter p) const {
        return this->m_valid[p];
    };

    void nShaderParams::SetArg(IEffect::Parameter p, const nShaderArg& arg) {
        this->m_args[p] = arg;
    };

    const nShaderArg& nShaderParams::GetArg(IEffect::Parameter p) const {
        return this->m_args[p];
    };

    void nShaderParams::SetInt(IEffect::Parameter p, int32_t v) {
        this->m_args[p].SetInt(v);
        this->m_valid[p] = true;
    };

    int32_t nShaderParams::GetInt(IEffect::Parameter p) const {
        return this->m_args[p].GetInt();
    };

    void nShaderParams::SetFloat(IEffect::Parameter p, float v) {
        this->m_args[p].SetFloat(v);
        this->m_valid[p] = true;
    };

    float nShaderParams::GetFloat(IEffect::Parameter p) const {
        return this->m_args[p].GetFloat();
    };

    void nShaderParams::SetFloat4(IEffect::Parameter p, const hta::nFloat4& v) {
        this->m_args[p].SetFloat4(v);
        this->m_valid[p] = true;
    };

    const hta::nFloat4& nShaderParams::GetFloat4(IEffect::Parameter p) const {
        return this->m_args[p].GetFloat4();
    };

    void nShaderParams::SetMatrix44(IEffect::Parameter p, const hta::CMatrix* v) {
        this->m_args[p].SetMatrix44(v);
        this->m_valid[p] = true;
    };

    const hta::CMatrix* nShaderParams::GetMatrix44(IEffect::Parameter p) const {
        return this->m_args[p].GetMatrix44();
    };

    void nShaderParams::SetTexture(IEffect::Parameter p, TexHandle v) {
        this->m_args[p].SetTexture(&v);
        this->m_valid[p] = true;
    };

    TexHandle nShaderParams::GetTexture(IEffect::Parameter p) {
        return *this->m_args[p].GetTexture();
    };

    void nShaderParams::SetVector4(IEffect::Parameter p, const hta::CVector4& v) {
        this->m_args[p].SetFloat4(v);
        this->m_valid[p] = true;
    };

    hta::CVector4 nShaderParams::GetVector4(IEffect::Parameter p) const {
        return this->m_args[p].GetFloat4();
    };

    void nShaderParams::Reset() {
        std::fill_n(this->m_args, this->SIZE, nShaderArg());
        std::fill_n(this->m_valid, this->SIZE, false);
    };

    HRESULT auxShaderInclude::Open(
        D3DXINCLUDE_TYPE IncludeType, const char* pName, const void* pParentData, const void** ppData, unsigned int* pBytes
    ) {
        hta::CStr fileName(pName);

        // Check if file exists, if not try "data/shaders/" prefix
        hta::m3d::fs::FileServer& fileServer = hta::m3d::Kernel::Instance()->GetFileServer();
        if (!fileServer.FileExists(fileName.m_charPtr)) {
            fileName = hta::CStr::format_("data/shaders/%s", pName);
        }

        // Open file stream
        hta::m3d::fs::FileStream* stream = fileServer.CreateFileStream();

        if (!stream->Open(fileName.m_charPtr, hta::m3d::fs::IStream::OPEN_READ)) {
            LOG_WARNING("auxShaderInclude: could not open include file '%s'", fileName.m_charPtr);
            delete stream;
            return E_FAIL;
        }

        // Read file contents
        unsigned int fileSize = stream->GetSize();
        void* data            = hta::m3d::Kernel::Instance()->g_mar.AllocMem(fileSize, 0, 0);
        stream->ReadBytes(data, fileSize);
        stream->Close();
        delete stream;

        // Return data to D3DX
        *ppData = data;
        *pBytes = fileSize;

        return S_OK;
    };

    HRESULT auxShaderInclude::Close(const void* pData) {
        if (pData) {
            hta::m3d::Kernel::Instance()->g_mar.FreeMem((void*)pData, 0, 0);
        }
        return S_OK;
    };

    static auxShaderInclude AUX_SHADER_INCLUDE{};

    EffectImpl::EffectImpl(bool bApplyGlobalParams) {
        this->m_bApplyGlobalParams = bApplyGlobalParams;
    };

    EffectImpl::~EffectImpl() {
        this->Invalidate();
        CDevice::Instance()->OnEffectDestructor(this);
    };

    bool EffectImpl::LoadFromFile(const char* filename, const std::vector<CompileParam>& params) {
        this->m_compileParams = params;

        auto stream = hta::m3d::Kernel::Instance()->GetFileServer().CreateFileStream();
        if (!stream->Open(filename, hta::m3d::fs::IStream::OPEN_READ)) {
            delete stream;
            return false;
        }

        size_t size = stream->GetSize();

        if (size == 0) {
            LOG_ERROR("Fail to compile EffectImpl '%s' %d", filename, __LINE__);
            delete stream;
            return false;
        }

        std::vector<char> buffer(size);

        stream->ReadBytes(buffer.data(), size);
        delete stream;

        CDevice* render = CDevice::Instance();

        std::vector<D3DXMACRO> macro(this->m_compileParams.size());
        for (size_t idx = 0; idx < this->m_compileParams.size(); idx++) {
            macro[idx].Name       = render->CompileParamToString(this->m_compileParams[idx]);
            macro[idx].Definition = "true";
        }
        macro.insert(macro.end(), render->m_d3dxMacros.begin(), render->m_d3dxMacros.end());
        macro.push_back({NULL, NULL});

        ID3DXBuffer* errorBuffer = nullptr;

        HRESULT error = D3DXCreateEffect(
            render->m_pd3dDevice,
            buffer.data(),
            static_cast<UINT>(buffer.size()),
            macro.data(),
            &AUX_SHADER_INCLUDE,
            D3DXSHADER_ENABLE_BACKWARDS_COMPATIBILITY,
            render->m_globalFxPool,
            &this->m_effect,
            &errorBuffer
        );

        if (FAILED(error)) {
            if (errorBuffer) {
                const char* errorText = (const char*)errorBuffer->GetBufferPointer();
                LOG_ERROR("Shader compilation error: %s\n%s", filename, errorText);
                errorBuffer->Release();
            } else {
                LOG_ERROR("D3DXCreateEffect failed with no error message");
            }

            LOG_ERROR("HRESULT: 0x%08X", error);
            return false;
        }

        this->mFileName = filename;
        this->m_effect->SetStateManager(&render->m_stateManager);
        this->m_hasBeenValidated = false;
        this->m_didNotValidate   = false;

        this->ValidateEffect();

        LOG_INFO("Load shader: %s", filename);
        return true;
    };

    bool EffectImpl::LoadFromString(const char* source, uint32_t size) {
        return false;
    };

    bool EffectImpl::IsValid() const {
        return this->m_hasBeenValidated;
    };

    uint32_t EffectImpl::GetNumTechniques() const {
        return this->m_numTechniques;
    };

    const IEffect::TechniqueDesc& EffectImpl::GetTechniqueDesc(uint32_t techId) const {
        return this->m_techDescs[techId].publicDesc;
    };

    void EffectImpl::SetCurTechnique(uint32_t techId) {
        if (techId != this->m_curTechnique) {
            this->UpdateParameterHandles();
            this->m_effect->SetTechnique(this->m_techDescs[techId].handle);
            this->m_curTechnique = techId;
        }
    };

    void EffectImpl::SetCurTechniqueByName(const char* techName) {
        size_t idx = 0;
        for (const auto& tech : this->m_techDescs) {
            idx++;
            if (tech.publicDesc.name == techName) {
                this->SetCurTechnique(idx);
                break;
            }
        }
    };

    uint32_t EffectImpl::GetCurTechnique() const {
        return this->m_curTechnique;
    };

    const char* EffectImpl::GetCurTechniqueName() const {
        uint32_t m_curTechnique = this->m_curTechnique;
        if (m_curTechnique < 0 || m_curTechnique >= this->m_numTechniques)
            return "";
        else
            return this->m_techDescs[m_curTechnique].publicDesc.name.c_str();
    };

    void EffectImpl::SetDefaultTechnique(bool preferPS20) {
        if (preferPS20 && this->m_defaultPS20Technique >= 0)
            this->SetCurTechnique(this->m_defaultPS20Technique);
        else
            this->SetCurTechnique(this->m_defaultTechnique);
    };

    bool EffectImpl::IsParameterUsed(IEffect::Parameter p) {
        return this->m_parameterHandles[p].d3dxHandle != 0;
    };

    void EffectImpl::SetInt(IEffect::Parameter p, int32_t v) {
        this->m_curParams.m_args[p].SetInt(v);
        this->m_effect->SetInt(this->m_parameterHandles[p].d3dxHandle, v);
    };

    void EffectImpl::SetFloat(IEffect::Parameter p, float v) {
        this->m_curParams.m_args[p].SetFloat(v);
        this->m_effect->SetFloat(this->m_parameterHandles[p].d3dxHandle, v);
    };

    void EffectImpl::SetVector4(IEffect::Parameter p, const hta::CVector4& v) {
        this->m_curParams.m_args[p].SetFloat4(v);
        this->m_effect->SetVector(this->m_parameterHandles[p].d3dxHandle, (D3DXVECTOR4*)&v);
    };

    void EffectImpl::SetVector3(IEffect::Parameter p, const hta::CVector& v) {
        hta::nFloat4 tmp{v.x, v.y, v.z, 1.0};
        this->m_curParams.m_args[p].SetFloat4(tmp);
        this->m_effect->SetVector(this->m_parameterHandles[p].d3dxHandle, (D3DXVECTOR4*)&tmp);
    };

    void EffectImpl::SetFloat4(IEffect::Parameter p, const hta::nFloat4& v) {
        this->m_effect->SetVector(this->m_parameterHandles[p].d3dxHandle, (D3DXVECTOR4*)&v);
    };

    void EffectImpl::SetMatrix(IEffect::Parameter p, const hta::CMatrix& v) {
        this->m_effect->SetMatrix(this->m_parameterHandles[p].d3dxHandle, (D3DXMATRIX*)&v);
    };

    void EffectImpl::SetTexture(IEffect::Parameter p, const TexHandle& v) {
        auto image = CDevice::Instance()->mActiveTextures.GetItem(v.m_handle);
        int frame  = CDevice::Instance()->GetTextureCurrentFrame(image, -1.0);
        this->m_effect->SetTexture(this->m_parameterHandles[p].d3dxHandle, image->m_maps[frame]->mHandleBase);
    };

    void EffectImpl::SetIntArray(IEffect::Parameter p, const int32_t* a, int32_t c) {
        this->m_effect->SetIntArray(this->m_parameterHandles[p].d3dxHandle, a, c);
    };

    void EffectImpl::SetFloatArray(IEffect::Parameter p, const float* a, int32_t c) {
        this->m_effect->SetFloatArray(this->m_parameterHandles[p].d3dxHandle, a, c);
    };

    void EffectImpl::SetFloat4Array(IEffect::Parameter p, const hta::nFloat4* a, int32_t c) {
        this->m_effect->SetVectorArray(this->m_parameterHandles[p].d3dxHandle, (D3DXVECTOR4*)a, c);
    };

    void EffectImpl::SetVector4Array(IEffect::Parameter p, const hta::CVector4* a, int32_t c) {
        this->m_effect->SetVectorArray(this->m_parameterHandles[p].d3dxHandle, (D3DXVECTOR4*)a, c);
    };

    void EffectImpl::SetMatrixArray(IEffect::Parameter p, const hta::CMatrix* a, int32_t c) {
        this->m_effect->SetMatrixArray(this->m_parameterHandles[p].d3dxHandle, (D3DXMATRIX*)a, c);
    };

    void EffectImpl::SetMatrixPointerArray(IEffect::Parameter p, const hta::CMatrix** a, int32_t c) {
        this->m_effect->SetMatrixPointerArray(this->m_parameterHandles[p].d3dxHandle, (const D3DXMATRIX**)a, c);
    };

    void EffectImpl::SetParams(nShaderParams& params) {
        this->m_effect->BeginParameterBlock();
        size_t idx = 0;
        for (auto& param : params.m_args) {
            if (!params.m_valid[idx])
                continue;

            auto handle = this->m_parameterHandles[idx].d3dxHandle;
            switch (param.m_type) {
            case nShaderArg::Int:
                m_effect->SetInt(handle, param.GetInt());
                break;
            case nShaderArg::Float:
                m_effect->SetFloat(handle, param.GetFloat());
                break;
            case nShaderArg::Float4:
                m_effect->SetVector(handle, (D3DXVECTOR4*)&param.GetFloat4());
                break;
            case nShaderArg::Matrix44:
                m_effect->SetMatrix(handle, (D3DXMATRIX*)param.GetMatrix44());
                break;
            case nShaderArg::Texture:
                m_effect->SetTexture(
                    handle, CDevice::Instance()->mActiveTextures.GetItem(param.GetTexture()->m_handle)->m_maps[0]->mHandleBase
                );
                break;
            default:
                break;
            }
            idx++;
        };
        D3DXHANDLE block = m_effect->EndParameterBlock();
        m_effect->ApplyParameterBlock(block);
    };

    int32_t EffectImpl::Begin() {
        if (this->m_didNotValidate)
            return 0;
        if (this->m_bApplyGlobalParams)
            this->ApplyGlobalFxParams();
        CDevice::Instance()->DuplicateBlend();
        CDevice::Instance()->DuplicateAlphaTest();
        uint32_t passes = 0;
        this->m_effect->Begin(&passes, 3u);
        return passes;
    };

    void EffectImpl::BeginPass(uint32_t pass) {
        this->m_effect->BeginPass(pass);
        this->m_effect->CommitChanges();
    };

    void EffectImpl::EndPass() {
        this->m_effect->EndPass();
    };

    void EffectImpl::End() {
        if (!this->m_didNotValidate) {
            this->m_effect->End();
            CDevice::Instance()->PopBlend();
            CDevice::Instance()->PopAlphaTest();
        }
    };

    void EffectImpl::CommitChanges() {
        this->m_effect->CommitChanges();
    };

    void EffectImpl::ApplyGlobalFxParams() {
        CDevice* dev       = CDevice::Instance();
        const char* handle = nullptr;

        if ((handle = m_parameterHandles[World].d3dxHandle)) {
            m_effect->SetMatrix(handle, (D3DXMATRIX*)&dev->GetModelMatrix());
        }

        if ((handle = m_parameterHandles[InvWorld].d3dxHandle)) {
            hta::CMatrix inv = ~dev->GetModelMatrix();
            m_effect->SetMatrix(handle, (D3DXMATRIX*)&inv);
        }

        if ((handle = m_parameterHandles[View].d3dxHandle)) {
            m_effect->SetMatrix(handle, (D3DXMATRIX*)&dev->MatGet());
        }

        if ((handle = m_parameterHandles[ModelView].d3dxHandle)) {
            hta::CMatrix mv = dev->MatGet() * dev->MatGetWorld();
            m_effect->SetMatrix(handle, (D3DXMATRIX*)&mv);
        }

        if ((handle = m_parameterHandles[Projection].d3dxHandle)) {
            m_effect->SetMatrix(handle, (D3DXMATRIX*)&dev->MatGetProj());
        }

        if ((handle = m_parameterHandles[ModelViewProjection].d3dxHandle)) {
            m_effect->SetMatrix(handle, (D3DXMATRIX*)&dev->GetModelViewProjMatrix());
        }

        if ((handle = m_parameterHandles[TmpLight0Dir].d3dxHandle)) {
            hta::CVector dir = (hta::CVector&)dev->m_lights[0].Direction;
            if (m_parameterHandles[TmpLight0Dir].space == 1) {
                dir *= dev->MatGetWorld();
            }
            if (m_parameterHandles[TmpLight0Dir].space == 2) {
                dir *= dev->GetModelMatrix();
            }

            m_effect->SetValue(handle, &dir, 12);
        }

        if ((handle = m_parameterHandles[ViewPos].d3dxHandle)) {
            hta::CVector4 viewPos(dev->GetViewOrigin(), 1.0f);

            if (m_parameterHandles[ViewPos].space == 1) {
                viewPos *= ~dev->GetModelMatrix();
            }

            m_effect->SetVector(handle, (D3DXVECTOR4*)&viewPos);
        }

        if ((handle = m_parameterHandles[NormalizationCubemap].d3dxHandle)) {
            m_effect->SetTexture(handle, dev->m_NormalizingCubemap);
        }
    };

    void EffectImpl::OnDeviceReset() {
        if (this->m_effect) {
            this->m_effect->OnLostDevice();
        }
    };

    void EffectImpl::OnDeviceRestore() {
        if (this->m_effect) {
            this->m_effect->OnResetDevice();
        }
    };

    void EffectImpl::Invalidate() {
        if (this->m_effect) {
            this->m_effect->Release();
            this->m_effect = nullptr;
        }
        this->m_hasBeenValidated = false;
        this->m_didNotValidate   = false;
        this->m_curTechnique     = -1;
        this->m_techDescs.clear();
        this->m_curParams.Reset();
    };

    static std::unordered_map<std::string_view, VertexType> VT_MAPPING = {
        {"VERTEX_XYZ", VERTEX_XYZ},
        {"VERTEX_XYZT1", VERTEX_XYZT1},
        {"VERTEX_XYZC", VERTEX_XYZC},
        {"VERTEX_XYZWC", VERTEX_XYZWC},
        {"VERTEX_XYZWCT1", VERTEX_XYZWCT1},
        {"VERTEX_XYZNC", VERTEX_XYZNC},
        {"VERTEX_XYZCT1", VERTEX_XYZCT1},
        {"VERTEX_XYZNT1", VERTEX_XYZNT1},
        {"VERTEX_XYZNCT1", VERTEX_XYZNCT1},
        {"VERTEX_XYZNCT2", VERTEX_XYZNCT2},
        {"VERTEX_XYZNT2", VERTEX_XYZNT2},
        {"VERTEX_XYZNT3", VERTEX_XYZNT3},
        {"VERTEX_XYZCT1_UVW", VERTEX_XYZCT1_UVW},
        {"VERTEX_XYZCT2_UVW", VERTEX_XYZCT2_UVW},
        {"VERTEX_XYZCT2", VERTEX_XYZCT2},
        {"VERTEX_XYZNT1T", VERTEX_XYZNT1T},
        {"VERTEX_XYZNCT1T", VERTEX_XYZNCT1T},
        {"VERTEX_XYZNCT1_UV2_S1", VERTEX_XYZNCT1_UV2_S1},
        {"VERTEX_STREAM_UV_S1", VERTEX_STREAM_UV_S1},
        {"VERTEX_WATERTEST", VERTEX_WATERTEST},
        {"VERTEX_GRASSTEST", VERTEX_GRASSTEST},
        {"VERTEX_IMPOSTORTEST", VERTEX_IMPOSTORTEST},
        {"VERTEX_YNI", VERTEX_YNI},
        {"VERTEX_XYZT1I", VERTEX_XYZT1I}
    };

    void EffectImpl::ValidateEffect() {
        D3DXEFFECT_DESC effectDesc;
        m_effect->GetDesc(&effectDesc);

        m_techDescs.clear();
        m_numTechniques = effectDesc.Techniques;

        // 1. Validate and collect techniques
        for (UINT i = 0; i < effectDesc.Techniques; ++i) {
            D3DXHANDLE tech = m_effect->GetTechnique(i);
            D3DXTECHNIQUE_DESC techDesc;
            m_effect->GetTechniqueDesc(tech, &techDesc);

            // Validate technique
            // if (FAILED(m_effect->ValidateTechnique(tech))) {
            //    LOG_ERROR("Could not validate technique '%s', skipping...", techDesc.Name);
            //    --m_numTechniques;
            //    continue;
            //}

            TechniqueDescInternal desc;
            desc.handle                      = tech;
            desc.publicDesc.name             = techDesc.Name;
            desc.publicDesc.numPasses        = techDesc.Passes;
            desc.publicDesc.briefDesc        = getStringAnnotation(tech, "Description", "N/A");
            desc.publicDesc.isDefault        = getBoolAnnotation(tech, "Default", false);
            desc.publicDesc.isPS20           = getBoolAnnotation(tech, "IsPs20", false);
            desc.publicDesc.useAlpha         = getBoolAnnotation(tech, "UseAlpha", true);
            desc.publicDesc.tangentSpaceUsed = getBoolAnnotation(tech, "ComputeTangentSpace", false);
            desc.publicDesc.maxInstances     = getIntAnnotation(tech, "MaxInstances", 0);

            // Check PS20 requirements
            if (desc.publicDesc.isPS20) {
                CDevice* dev = CDevice::Instance();

                // Check CVars and hardware support
                // TODO: Check m_r_allowPS20 and m_r_allowPS20ForNV30 CVars
                // For now, just check hardware:
                if (!dev->IsFeatureSupported(FEATURE_PS_2_0)) {
                    LOG_WARNING("PS20 technique '%s' skipped - no hardware support", techDesc.Name);
                    --m_numTechniques;
                    continue;
                }
            }

            // Vertex format
            hta::CStr vfStr = getStringAnnotation(tech, "VertexFormat", "");
            if (VT_MAPPING.find(vfStr.c_str()) == VT_MAPPING.end()) {
                LOG_ERROR("Unsupported vertex format '%s' in technique '%s', skipping...", vfStr.c_str(), techDesc.Name);
                --m_numTechniques;
                continue;
            }
            desc.publicDesc.vertexFormat = VT_MAPPING.at(vfStr.c_str());

            m_techDescs.push_back(desc);
        }

        // 2. Find default techniques (original logic)
        m_defaultTechnique     = -1;
        m_defaultPS20Technique = -1;

        for (UINT i = 0; i < m_techDescs.size(); ++i) {
            if (m_techDescs[i].publicDesc.isDefault) {
                m_defaultTechnique = i;
                if (m_techDescs[i].publicDesc.isPS20) {
                    m_defaultPS20Technique = i;
                }
            }
        }

        // Fallback
        if (m_defaultTechnique == -1 && !m_techDescs.empty()) {
            LOG_WARNING("No default technique found, using first one ('%s')", m_techDescs[0].publicDesc.name.c_str());
            m_defaultTechnique = 0;
        }

        m_hasBeenValidated = true;
        m_didNotValidate   = false;
    }

    void EffectImpl::UpdateParameterHandles() {
        memset(m_parameterHandles, 0, sizeof(m_parameterHandles));
        m_effect->GetCurrentTechnique();

        for (int i = 0; i < 49; i++) {
            const char* semantic = CDevice::Instance()->EffectParameterToString((IEffect::Parameter)i);
            D3DXHANDLE handle    = m_effect->GetParameterBySemantic(nullptr, semantic);

            if (!handle)
                continue;

            m_parameterHandles[i].d3dxHandle = handle;

            D3DXPARAMETER_DESC paramDesc;
            if (FAILED(m_effect->GetParameterDesc(handle, &paramDesc))) {
                continue;
            }

            m_parameterHandles[i].isShared = paramDesc.Flags & 1;

            // Parse annotations
            for (UINT j = 0; j < paramDesc.Annotations; j++) {
                D3DXHANDLE annot = m_effect->GetAnnotation(handle, j);
                D3DXPARAMETER_DESC annotDesc;

                if (FAILED(m_effect->GetParameterDesc(annot, &annotDesc)))
                    continue;

                hta::CStr name(annotDesc.Name);

                if (name == "SPACE" && annotDesc.Class == D3DXPC_SCALAR && annotDesc.Type == D3DXPT_INT) {
                    m_effect->GetInt(annot, &m_parameterHandles[i].space);
                }
            }
        }
    };

    bool EffectImpl::getBoolAnnotation(D3DXHANDLE tech, const char* name, bool defaultValue) {
        D3DXHANDLE annot = m_effect->GetAnnotationByName(tech, name);
        if (!annot)
            return defaultValue;

        D3DXPARAMETER_DESC desc;
        m_effect->GetParameterDesc(annot, &desc);
        if (desc.Type != D3DXPT_BOOL)
            return defaultValue;

        BOOL value;
        m_effect->GetBool(annot, &value);
        return value != 0;
    };

    int EffectImpl::getIntAnnotation(D3DXHANDLE tech, const char* name, int defaultValue) {
        D3DXHANDLE annot = m_effect->GetAnnotationByName(tech, name);
        if (!annot)
            return defaultValue;

        D3DXPARAMETER_DESC desc;
        m_effect->GetParameterDesc(annot, &desc);
        if (desc.Type != D3DXPT_INT)
            return defaultValue;

        int value;
        m_effect->GetInt(annot, &value);
        return value;
    };

    hta::CStr EffectImpl::getStringAnnotation(D3DXHANDLE tech, const char* name, const hta::CStr& defaultValue) {
        D3DXHANDLE annot = m_effect->GetAnnotationByName(tech, name);
        if (!annot)
            return hta::CStr(defaultValue);

        D3DXPARAMETER_DESC desc;
        m_effect->GetParameterDesc(annot, &desc);
        if (desc.Type != D3DXPT_STRING)
            return defaultValue;

        const char* value;
        m_effect->GetString(annot, &value);
        return hta::CStr(value);
    };

    void Tokenize(const hta::CStr& str, std::vector<hta::CStr>& tokens, const char* delimiters) {
        tokens.clear();

        if (!str.m_charPtr || !strlen(str.m_charPtr))
            return;

        // Copy string for strtok (modifies input)
        size_t len   = strlen(str.m_charPtr);
        char* buffer = new char[len + 1];
        strcpy(buffer, str.m_charPtr);

        char* token = strtok(buffer, delimiters);
        while (token) {
            tokens.push_back(hta::CStr(token));
            token = strtok(nullptr, delimiters);
        }

        delete[] buffer;
    }

    void EffectImpl::getUserRenderParams(const hta::CStr& userParamsStr, std::vector<UserParam>& userParams) {
        userParams.clear();

        if (!userParamsStr.m_charPtr || !strlen(userParamsStr.m_charPtr))
            return;

        std::vector<hta::CStr> tokens;
        Tokenize(userParamsStr, tokens, ",");

        for (const auto& token : tokens) {
            std::vector<hta::CStr> paramData;
            Tokenize(token, paramData, " \t");

            if (paramData.size() < 2)
                continue;

            UserParam param;
            param.name = paramData[0];

            if (paramData[1] == "MATRIX")
                param.type = EffectImpl::UserParam::UPT_MATRIX;
            else if (paramData[1] == "VECTOR4")
                param.type = EffectImpl::UserParam::UPT_VECTOR4;
            else if (paramData[1] == "VECTOR")
                param.type = EffectImpl::UserParam::UPT_VECTOR;
            else if (paramData[1] == "FLOAT")
                param.type = EffectImpl::UserParam::UPT_FLOAT;

            userParams.push_back(param);
        }
    };

    bool PoolFieldInfo::operator==(const PoolFieldInfo& other) {
        return this->Offset == other.Offset && this->Size == other.Size;
    };

    HlslShaderImpl::~HlslShaderImpl() {
        this->Invalidate();
        CDevice::Instance()->OnHlslShaderDestructor(this);
    };

    std::set<IHlslShader::Profile> PS_SHADERS = {
        IHlslShader::PS_1_1, IHlslShader::PS_1_3, IHlslShader::PS_1_4, IHlslShader::PS_2_0, IHlslShader::PS_2_0, IHlslShader::PS_3_0
    };

    const char* SM_ALIAS[] = {
        "vs_1_1",
        "vs_2_0",
        "vs_3_0",
        "ps_1_1",
        "ps_1_3",
        "ps_1_4",
        "ps_2_0",
        "ps_2_a",
        "ps_3_0",
    };

    bool HlslShaderImpl::LoadFromFile(
        const char* filename, const char* entry, IHlslShader::Profile profile, const std::vector<CompileParam>& params
    ) {
        CDevice* render = CDevice::Instance();
        bool is_pixel   = PS_SHADERS.find(profile) != PS_SHADERS.end();
        if (is_pixel && render->IsFeatureSupported(FEATURE_PS_2_0))
            profile = profile >= PS_2_0 ? PS_3_0 : PS_2_0;

        auto stream = hta::m3d::Kernel::Instance()->GetFileServer().CreateFileStream();
        if (!stream->Open(filename, hta::m3d::fs::IStream::OPEN_READ)) {
            delete stream;
            return false;
        }

        size_t size = stream->GetSize();

        if (size == 0) {
            delete stream;
            return false;
        }

        std::vector<char> buffer(size);

        stream->ReadBytes(buffer.data(), size);
        delete stream;

        std::vector<D3DXMACRO> macro(this->m_compileParams.size());
        for (size_t idx = 0; idx < this->m_compileParams.size(); idx++) {
            macro[idx].Name       = render->CompileParamToString(this->m_compileParams[idx]);
            macro[idx].Definition = "true";
        }
        macro.insert(macro.end(), render->m_d3dxMacros.begin(), render->m_d3dxMacros.end());
        macro.push_back({NULL, NULL});

        LPD3DXBUFFER object;
        LPD3DXBUFFER errors = nullptr;
        HRESULT error       = D3DXCompileShader(
            buffer.data(),
            buffer.size(),
            macro.data(),
            &AUX_SHADER_INCLUDE,
            entry,
            SM_ALIAS[profile],
            D3DXSHADER_SKIPOPTIMIZATION,
            &object,
            &errors,
            &this->m_constantTable
        );

        if (FAILED(error)) {
            if (errors) {
                LOG_ERROR("Shader %s compile error: %s", filename, (char*)errors->GetBufferPointer());
                errors->Release();
            }
            if (object)
                object->Release();
            return false;
        }

        if (is_pixel)
            render->m_pd3dDevice->CreatePixelShader((const DWORD*)object->GetBufferPointer(), &this->m_ps);
        else
            render->m_pd3dDevice->CreateVertexShader((const DWORD*)object->GetBufferPointer(), &this->m_vs);

        object->Release();

        D3DXCONSTANTTABLE_DESC desc;
        this->m_constantTable->GetDesc(&desc);
        this->m_constantTable->SetDefaults(render->m_pd3dDevice);
        this->m_numConstants = desc.Constants;
        this->mFileName      = filename;
        this->m_profile      = profile;

        return true;
    };

    bool HlslShaderImpl::LoadFromString(const char*, uint32_t, const char*, IHlslShader::Profile) {
        return false;
    };

    bool HlslShaderImpl::IsValid() const {
        return false;
    };

    uint32_t HlslShaderImpl::GetNumberOfParams() const {
        return this->m_numConstants;
    };

    uint32_t HlslShaderImpl::GetParamHandleByName(const char* name) {
        return (uint32_t)this->m_constantTable->GetConstantByName(0, name);
    };

    void HlslShaderImpl::SetInt(uint32_t p, int32_t v) {
        this->m_constantTable->SetInt(CDevice::Instance()->m_pd3dDevice, (D3DXHANDLE)p, v);
    };

    void HlslShaderImpl::SetFloat(uint32_t p, float v) {
        this->m_constantTable->SetFloat(CDevice::Instance()->m_pd3dDevice, (D3DXHANDLE)p, v);
    };

    void HlslShaderImpl::SetVector4(uint32_t p, const hta::CVector4& v) {
        this->m_constantTable->SetVector(CDevice::Instance()->m_pd3dDevice, (D3DXHANDLE)p, (D3DXVECTOR4*)&v);
    };

    void HlslShaderImpl::SetVector3(uint32_t p, const hta::CVector& v) {
        hta::nFloat4 t = {v.x, v.y, v.z, 1.0};
        this->m_constantTable->SetVector(CDevice::Instance()->m_pd3dDevice, (D3DXHANDLE)p, (D3DXVECTOR4*)&t);
    };

    void HlslShaderImpl::SetFloat4(uint32_t p, const hta::nFloat4& v) {
        this->m_constantTable->SetVector(CDevice::Instance()->m_pd3dDevice, (D3DXHANDLE)p, (D3DXVECTOR4*)&v);
    };

    void HlslShaderImpl::SetMatrix(uint32_t p, const hta::CMatrix& v) {
        this->m_constantTable->SetMatrix(CDevice::Instance()->m_pd3dDevice, (D3DXHANDLE)p, (D3DXMATRIX*)&v);
    };

    void HlslShaderImpl::SetIntArray(uint32_t p, const int32_t* v, int32_t s) {
        this->m_constantTable->SetIntArray(CDevice::Instance()->m_pd3dDevice, (D3DXHANDLE)p, v, s);
    };

    void HlslShaderImpl::SetFloatArray(uint32_t p, const float* v, int32_t s) {
        this->m_constantTable->SetFloatArray(CDevice::Instance()->m_pd3dDevice, (D3DXHANDLE)p, v, s);
    };

    void HlslShaderImpl::SetFloat4Array(uint32_t p, const hta::nFloat4* v, int32_t s) {
        this->m_constantTable->SetVectorArray(CDevice::Instance()->m_pd3dDevice, (D3DXHANDLE)p, (D3DXVECTOR4*)v, s);
    };

    void HlslShaderImpl::SetVector4Array(uint32_t p, const hta::CVector4* v, int32_t s) {
        this->m_constantTable->SetVectorArray(CDevice::Instance()->m_pd3dDevice, (D3DXHANDLE)p, (D3DXVECTOR4*)v, s);
    };

    void HlslShaderImpl::SetMatrixArray(uint32_t p, const hta::CMatrix* v, int32_t s) {
        this->m_constantTable->SetMatrixArray(CDevice::Instance()->m_pd3dDevice, (D3DXHANDLE)p, (D3DXMATRIX*)v, s);
    };

    void HlslShaderImpl::SetMatrixPointerArray(uint32_t p, const hta::CMatrix** v, int32_t s) {
        this->m_constantTable->SetMatrixPointerArray(CDevice::Instance()->m_pd3dDevice, (D3DXHANDLE)p, (const D3DXMATRIX**)v, s);
    };

    void HlslShaderImpl::Apply() {
        static wchar_t marker[256];
        swprintf(marker, 256, L"SetShader %S", this->mFileName.c_str());
        D3DPERF_SetMarker(D3DCOLOR_XRGB(0, 255, 0), marker);

        if (this->m_profile > IHlslShader::VS_3_0)
            CDevice::Instance()->setPixelShader(this->m_ps);
        else
            CDevice::Instance()->setVertexShader(this->m_vs);
    };

    void HlslShaderImpl::UpdateShaderInfo() {
        D3DXCONSTANTTABLE_DESC desc;
        this->m_constantTable->GetDesc(&desc);
        this->m_numConstants = desc.Constants;
        this->m_constantTable->SetDefaults(CDevice::Instance()->m_pd3dDevice);
    };

    bool HlslShaderImpl::IsValidParam(uint32_t) const {
        return true;
    };

    void HlslShaderImpl::OnDeviceReset() {};

    void HlslShaderImpl::OnDeviceRestore() {};

    void HlslShaderImpl::Invalidate() {
        if (this->m_shader) {
            if (this->m_profile > IHlslShader::VS_3_0) {
                this->m_ps->Release();
            } else {
                this->m_vs->Release();
            }
            this->m_shader = nullptr;
        }

        if (this->m_constantTable) {
            this->m_constantTable->Release();
            this->m_constantTable = nullptr;
        }
    };

    void HlslShaderImpl::ValidateEffect() {};

    bool HlslShaderImpl::IsVertexShader() const {
        return this->m_profile <= VS_3_0;
    };

    AsmShaderImpl::~AsmShaderImpl() {
        this->Invalidate();
        CDevice::Instance()->OnAsmShaderDestructor(this);
    };

    bool AsmShaderImpl::LoadFromFile(const char* filename, IAsmShader::Type type) {
        CDevice* render = CDevice::Instance();

        auto stream = hta::m3d::Kernel::Instance()->GetFileServer().CreateFileStream();
        if (!stream->Open(filename, hta::m3d::fs::IStream::OPEN_READ)) {
            delete stream;
            return false;
        }

        size_t size = stream->GetSize();

        if (size == 0) {
            LOG_ERROR("Fail to compile AsmShaderImpl '%s' %d", filename, __LINE__);
            delete stream;
            return false;
        }

        std::vector<char> buffer(size);

        stream->ReadBytes(buffer.data(), size);
        delete stream;

        LPD3DXBUFFER object;
        HRESULT error = D3DXAssembleShader(buffer.data(), buffer.size(), NULL, &AUX_SHADER_INCLUDE, 0, &object, NULL);

        if (FAILED(error)) {
            if (object)
                object->Release();
            LOG_ERROR("Fail to compile AsmShaderImpl '%s' %d", filename, __LINE__);
            return false;
        }

        this->m_type    = type;
        this->mFileName = filename;

        switch (this->m_type) {
        case IAsmShader::VERTEX_SHADER:
            render->m_pd3dDevice->CreateVertexShader((const DWORD*)object->GetBufferPointer(), &this->m_vs);
            break;
        case IAsmShader::PIXEL_SHADER:
            render->m_pd3dDevice->CreatePixelShader((const DWORD*)object->GetBufferPointer(), &this->m_ps);
            break;
        }

        object->Release();

        return true;
    };

    bool AsmShaderImpl::LoadFromString(const char*, uint32_t, IAsmShader::Type) {
        return false;
    };

    bool AsmShaderImpl::IsValid() const {
        return false;
    };

    void AsmShaderImpl::Apply() {
        switch (this->m_type) {
        case IAsmShader::VERTEX_SHADER:
            CDevice::Instance()->setVertexShader(this->m_vs);
            break;
        case IAsmShader::PIXEL_SHADER:
            CDevice::Instance()->setPixelShader(this->m_ps);
            break;
        }
    };

    void AsmShaderImpl::OnDeviceReset() {};

    void AsmShaderImpl::OnDeviceRestore() {};

    void AsmShaderImpl::Invalidate() {
        if (this->m_shader) {
            switch (this->m_type) {
            case IAsmShader::VERTEX_SHADER:
                this->m_vs->Release();
                break;
            case IAsmShader::PIXEL_SHADER:
                this->m_ps->Release();
                break;
            }
        }
    };

    bool AsmShaderImpl::IsVertexShader() const {
        return this->m_type == VERTEX_SHADER;
    };

    bool CDevice::ShaderIdData::operator<(const CDevice::ShaderIdData& rhs) const {
        if (filename == rhs.filename)
            return std::lexicographical_compare(
                compileParams.begin(), compileParams.end(), rhs.compileParams.begin(), rhs.compileParams.end()
            );
        return filename < rhs.filename;
    };

    void CDevice::DXCursorInfo::SetUp(const TexHandle& tex, int32_t x, int32_t y, int32_t f) {
        this->m_texId    = tex;
        this->m_xHotSpot = x;
        this->m_yHotSpot = y;
        this->m_frame    = f;
    };

    void CDevice::_SetLastResult(HRESULT result) {
        this->m_lastResult = result;
        assert(SUCCEEDED(result) && "DX Error catched!");
    };

    int32_t CDevice::DecRef() {
        this->m_refCount--;
        if (this->m_parent)
            this->m_parent->DecRef();
        if (this->m_refCount <= 0)
            delete this;
        return 0;
    };

    int32_t CDevice::IncRef() {
        if (this->m_parent)
            this->m_parent->IncRef();
        return 0;
    };

    void* CDevice::QueryIface(const char* iface) {
        if (this->m_parent)
            return this->m_parent->QueryIface(iface);
        return nullptr;
    };

    void CDevice::InitMatrices() {
        this->m_matViewStackTop = 0;
        this->m_matViewStack[0].Identity();

        this->m_matProjStackTop = 0;
        this->m_matProjStack[0].Identity();

        this->m_matWorldStackTop = 0;
        this->m_matWorldStack[0].Identity();

        this->m_fastClipEnabled              = false;
        this->m_matViewIsNotActuated         = true;
        this->m_matInvViewIsNotActuated      = true;
        this->m_matWorldIsNotActuated        = true;
        this->m_updateModelViewProj          = true;
        this->m_updateModelViewProjWithWorld = true;
        this->m_updateModelMatrix            = true;
    };

    void CDevice::rstStacks() {
        this->m_stackTopAlphaTest = 0;
        this->m_stackAlphaTest[0] = 0;
        this->SetAlphaTest(0, 0);

        this->m_stackTopBlend = 0;
        this->m_stackBlend[0] = BM_NONE;
        this->SetBlend(BM_NONE, 0);

        this->m_stackTopZbState = 0;
        this->m_stackZbState[0] = ZB_ENABLE;
        this->SetZbState(ZB_ENABLE, 0);

        this->m_stackTopCull = 0;
        this->m_stackCull[0] = M3DCULL_NONE;
        this->SetCull(M3DCULL_NONE, 0);

        this->m_stackTopZFunc = 0;
        this->m_stackZFunc[0] = M3DCMP_LESSEQUAL;
        this->SetZFunc(M3DCMP_LESSEQUAL, 0);

        this->m_stackTopLighting = 0;
        this->m_stackLighting[0] = 0;
        this->SetLighting(0, 0);

        this->m_stackTopAmbient = 0;
        this->m_stackAmbient[0] = 0;
        this->SetAmbient(0, 0);

        this->m_stackTopFog = 0;
        this->m_stackFog[0] = 0;
        this->SetFog(0, 0);

        this->m_stackTopFogColor = 0;
        this->m_stackFogColor[0] = 0;
        this->SetFogColor(0, 0);

        this->m_stackTopFogMode = 0;
        this->m_stackFogMode[0] = M3DFOG_NONE;
        this->SetFogMode(M3DFOG_NONE, 0);

        this->m_stackTopFogStart = 0;
        this->m_stackFogStart[0] = 0.0;
        this->SetFogStart(0.0, 0);

        this->m_stackTopFogEnd = 0;
        this->m_stackFogEnd[0] = 0.0;
        this->SetFogEnd(0.0, 0);

        this->m_stackTopFillMode = 0;
        this->m_stackFillMode[0] = M3DFILL_SOLID;
        this->SetFillMode(M3DFILL_SOLID, 0);

        this->m_stackTopZBias = 0;
        this->m_stackZBias[0] = 0.0;
        this->SetZBias(0.0, 0);

        this->m_stackTopZBiasSlopeScale = 0;
        this->m_stackZBiasSlopeScale[0] = 0.0;
        this->SetZBiasSlopeScale(0.0, 0);

        this->m_stackTopShadeMode = 0;
        this->m_stackShadeMode[0] = M3DSHADE_GOURAUD;
        this->SetShadeMode(M3DSHADE_GOURAUD, 0);

        this->m_stackTopPointSpriteEnable = 0;
        this->m_stackPointSpriteEnable[0] = 0;
        this->SetPointSpriteEnable(0, 0);

        this->m_stackTopPointScaleEnable = 0;
        this->m_stackPointScaleEnable[0] = 0;
        this->SetPointScaleEnable(0, 0);

        this->m_stackTopPointSizeMin = 0;
        this->m_stackPointSizeMin[0] = 1.0f;
        this->SetPointSizeMin(1.0, 0);

        this->m_stackTopPointSizeMax = 0;
        this->m_stackPointSizeMax[0] = 1.0f;
        this->SetPointSizeMax(1.0, 0);

        this->m_stackTopPointSize = 0;
        this->m_stackPointSize[0] = 1.0f;
        this->SetPointSize(1.0, 0);

        this->m_stackTopPointScaleA = 0;
        this->m_stackPointScaleA[0] = 1.0f;
        this->SetPointScaleA(1.0, 0);

        this->m_stackTopPointScaleB = 0;
        this->m_stackPointScaleB[0] = 0.0;
        this->SetPointScaleB(0.0, 0);

        this->m_stackTopPointScaleC = 0;
        this->m_stackPointScaleC[0] = 0.0;
        this->SetPointScaleC(0.0, 0);

        this->m_stackTopTFactor = 0;
        this->m_stackTFactor[0] = 0;
        this->SetTFactor(0, 0);

        this->m_stackTopLocalViewer = 0;
        this->m_stackLocalViewer[0] = 0;
        this->SetLocalViewer(0, 0);

        this->m_stackTopSpecularLighting = 0;
        this->m_stackSpecularLighting[0] = 0;
        this->SetSpecularLighting(0, 0);

        this->m_stackTopColorWriteMask = 0;
        this->m_stackColorWriteMask[0] = -1;
        this->SetColorWriteMask(-1u, 0);

        this->m_stackTopNPatchLevel = 0;
        this->m_stackNPatchLevel[0] = 0.0;
        this->SetNPatchLevel(0.0, 0);

        this->m_stackTopStencilState = 0;
        this->m_stackStencilState[0] = 0;
        this->SetStencilState(0, 0);

        this->m_stackTopStencilMask = 0;
        this->m_stackStencilMask[0] = -1;
        this->SetStencilMask(-1u, 0);

        this->m_stackTopDithering = 0;
        this->m_stackDithering[0] = 0;
        this->SetDithering(0, 0);

        this->m_stackTopStencilRef = 0;
        this->m_stackStencilRef[0] = 0;
        this->SetStencilRef(0, 0);

        this->m_stackTopStencilWriteMask = 0;
        this->m_stackStencilWriteMask[0] = -1;
        this->SetStencilWriteMask(-1u, 0);

        this->m_stackTopStencilFunc = 0;
        this->m_stackStencilFunc[0] = M3DCMP_ALWAYS;
        this->SetStencilFunc(M3DCMP_ALWAYS, 0);

        this->m_stackTopStencilFail = 0;
        this->m_stackStencilFail[0] = OP_KEEP;
        this->SetStencilFail(OP_KEEP, 0);

        this->m_stackTopStencilZFail = 0;
        this->m_stackStencilZFail[0] = OP_KEEP;
        this->SetStencilZFail(OP_KEEP, 0);

        this->m_stackTopStencilPass = 0;
        this->m_stackStencilPass[0] = OP_KEEP;
        this->SetStencilPass(OP_KEEP, 0);

        this->m_stackTopStencil2SidedEnable = 0;
        this->m_stackStencil2SidedEnable[0] = 0;
        this->SetStencil2SidedEnable(0, 0);

        this->m_stackTopStencilCcwFunc = 0;
        this->m_stackStencilCcwFunc[0] = M3DCMP_ALWAYS;
        this->SetStencilCcwFunc(M3DCMP_ALWAYS, 0);

        this->m_stackTopStencilCcwFail = 0;
        this->m_stackStencilCcwFail[0] = OP_KEEP;
        this->SetStencilCcwFail(OP_KEEP, 0);

        this->m_stackTopStencilCcwZFail = 0;
        this->m_stackStencilCcwZFail[0] = OP_KEEP;
        this->SetStencilCcwZFail(OP_KEEP, 0);

        this->m_stackTopStencilCcwPass = 0;
        this->m_stackStencilCcwPass[0] = OP_KEEP;
        this->SetStencilCcwPass(OP_KEEP, 0);

        this->m_stackTopMultiSample = 0;
        this->m_stackMultiSample[0] = 0;
        this->SetMultiSample(0, 0);

        this->m_stackTopMultiSampleMask = 0;
        this->m_stackMultiSampleMask[0] = -1;
        this->SetMultiSampleMask(-1u, 0);
    };

    const char* FEATURE_LABELS[FEATURE_NUM_FEATURES] = {
        "VS_1_1",
        "VS_2_0",
        "VS_3_0",
        "PS_1_1",
        "PS_1_3",
        "PS_1_4",
        "PS_2_0",
        "PS_3_0",
        "STENCIL",
        "2SIDED_STENCIL",
        "NON_POW2_RT",
        "MRT",
        "NON_POW2_CONDITIONAL",
        "QUERY_VCACHE",
        "QUERY_EVENT",
        "QUERY_OCCLUSION",
        "QUERY_TIMESTAMP",
        "QUERY_TIMESTAMPDISJOINT",
        "QUERY_TIMESTAMPFREQ",
        "QUERY_PIPELINETIMINGS",
        "QUERY_INTERFACETIMINGS",
        "QUERY_VERTEXTIMINGS",
        "QUERY_PIXELTIMINGS",
        "QUERY_BANDWIDTHTIMINGS",
        "QUERY_CACHEUTILIZATION",
    };

    void CDevice::logCaps() {
        LOG_DEBUG("Device info:");
        LOG_DEBUG("  Texture Limit:    %u x %u", m_d3dCaps.MaxTextureWidth, m_d3dCaps.MaxTextureHeight);
        LOG_DEBUG(
            "  Guard Band:       %.1f, %.1f, %.1f, %.1f",
            m_d3dCaps.GuardBandLeft,
            m_d3dCaps.GuardBandTop,
            m_d3dCaps.GuardBandRight,
            m_d3dCaps.GuardBandBottom
        );
        LOG_DEBUG("  Blend Stages:     %u", m_d3dCaps.MaxTextureBlendStages);
        LOG_DEBUG("  Sampler Limit:    %u", m_d3dCaps.MaxSimultaneousTextures);
        LOG_DEBUG("  Light Limit:      %u", m_d3dCaps.MaxActiveLights);
        LOG_DEBUG("  Clip Plane Limit: %u", m_d3dCaps.MaxUserClipPlanes);
        LOG_DEBUG("  SM (Vertex):      0x%x", m_d3dCaps.VertexShaderVersion);
        LOG_DEBUG("  SM (Pixel):       0x%x", m_d3dCaps.PixelShaderVersion);
        LOG_DEBUG("  Features:");

        for (int feat = FEATURE_VS_1_1; feat < FEATURE_NUM_FEATURES; feat++) {
            LOG_DEBUG("    %-30s: %s", FEATURE_LABELS[feat], IsFeatureSupported((DeviceFeature)feat) ? "true" : "false");
        }
    };

    bool CDevice::defineInstancingSupport() {
        if (m_d3dCaps.VertexShaderVersion < D3DVS_VERSION(3, 0))
            return false;

        if (FAILED(m_pD3D->CheckDeviceFormat(
                0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_SURFACE, (D3DFORMAT)MAKEFOURCC('I', 'N', 'S', 'T')
            )))
            return false;

        m_pd3dDevice->SetRenderState(D3DRS_POINTSIZE, MAKEFOURCC('I', 'N', 'S', 'T'));
        return true;
    };

    int32_t CDevice::SetXFormMatrix(int32_t num, const hta::CMatrix& mat) {
        ++this->m_stats.swMatrices;
        return this->m_pd3dDevice->SetTransform((D3DTRANSFORMSTATETYPE)num, (D3DXMATRIX*)&mat);
    };

    static const D3DVERTEXELEMENT9 ddXYZCT1[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddXYZT1[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddXYZNT1[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddXYZNT2[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 32, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddXYZNT3[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 32, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        {0, 40, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddXYZN[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddXYZ[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddXYZW[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddXYZC[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddXYZNC[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddXYZWCT1[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddXYZWC[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddXYZNCT1[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddXYZNCT2[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 36, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddXYZCT2[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddXYZNCT1T[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 36, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 0},
        D3DDECL_END(),
    };

    using vXYZNT1T                             = hta::m3d::rend::VertexXYZNT1T;
    static const D3DVERTEXELEMENT9 ddXYZNT1T[] = {
        {0, offsetof(vXYZNT1T, position), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, offsetof(vXYZNT1T, normal), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {0, offsetof(vXYZNT1T, uv0), D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, offsetof(vXYZNT1T, tangent), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 0},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddXYZCT1_UVW[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 16, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddXYZCT2_UVW[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 16, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddXYZNCT1_UV2_S1[] = {
        {0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, // Single float!
        {0, 4, D3DDECLTYPE_SHORT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0}, // Unsigned shorts!
        {1, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddWaterTest[] = {
        {0, 0, D3DDECLTYPE_SHORT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddGrassTest[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddImpostorTest[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddYNI[] = {
        {0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 4, D3DDECLTYPE_SHORT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 ddXYZT1I[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 20, D3DDECLTYPE_SHORT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        D3DDECL_END(),
    };
    static const D3DVERTEXELEMENT9 ddInstanceId[] = {
        {1, 0, D3DDECLTYPE_SHORT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 1},
        D3DDECL_END(),
    };

    void CDevice::InitVertexDeclarations() {
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZCT1, &this->m_vdXYZCT1));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZT1, &this->m_vdXYZT1));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZNT1, &this->m_vdXYZNT1));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZNT2, &this->m_vdXYZNT2));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZNT3, &this->m_vdXYZNT3));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZN, &this->m_vdXYZN));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZ, &this->m_vdXYZ));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZW, &this->m_vdXYZW));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZC, &this->m_vdXYZC));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZNC, &this->m_vdXYZNC));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZWCT1, &this->m_vdXYZWCT1));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZWC, &this->m_vdXYZWC));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZNCT1, &this->m_vdXYZNCT1));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZNCT2, &this->m_vdXYZNCT2));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZCT2, &this->m_vdXYZCT2));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZNCT1T, &this->m_vdXYZNCT1T));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZNT1T, &this->m_vdXYZNT1T));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZCT1_UVW, &this->m_vdXYZCT1_UVW));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZCT2_UVW, &this->m_vdXYZCT2_UVW));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZNCT1_UV2_S1, &this->m_vdXYZNCT1_UV2_S1));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddWaterTest, &this->m_vdWaterTest));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddGrassTest, &this->m_vdGrassTest));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddImpostorTest, &this->m_vdImpostorTest));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddYNI, &this->m_vdYNI));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddXYZT1I, &this->m_vdXYZT1I));
        _SetLastResult(this->m_pd3dDevice->CreateVertexDeclaration(ddInstanceId, &this->m_vdInstanceId));

        this->m_vdXYZW4NCT1  = nullptr;
        this->m_vdXYZW4TNCT1 = nullptr;

        this->CreateCombinedVertexDeclaration(this->m_vdXYZCT1, this->m_vdInstanceId);
        this->CreateCombinedVertexDeclaration(this->m_vdXYZT1, this->m_vdInstanceId);
        this->CreateCombinedVertexDeclaration(this->m_vdXYZNT1, this->m_vdInstanceId);
        this->CreateCombinedVertexDeclaration(this->m_vdXYZNT2, this->m_vdInstanceId);
        this->CreateCombinedVertexDeclaration(this->m_vdXYZNT3, this->m_vdInstanceId);
        this->CreateCombinedVertexDeclaration(this->m_vdXYZN, this->m_vdInstanceId);
        this->CreateCombinedVertexDeclaration(this->m_vdXYZ, this->m_vdInstanceId);
        this->CreateCombinedVertexDeclaration(this->m_vdXYZW, this->m_vdInstanceId);
        this->CreateCombinedVertexDeclaration(this->m_vdXYZC, this->m_vdInstanceId);
        this->CreateCombinedVertexDeclaration(this->m_vdXYZNC, this->m_vdInstanceId);
        this->CreateCombinedVertexDeclaration(this->m_vdXYZWCT1, this->m_vdInstanceId);
        this->CreateCombinedVertexDeclaration(this->m_vdXYZWC, this->m_vdInstanceId);
        this->CreateCombinedVertexDeclaration(this->m_vdXYZNCT1, this->m_vdInstanceId);
        this->CreateCombinedVertexDeclaration(this->m_vdXYZNCT2, this->m_vdInstanceId);
        this->CreateCombinedVertexDeclaration(this->m_vdXYZCT2, this->m_vdInstanceId);
        this->CreateCombinedVertexDeclaration(this->m_vdXYZNCT1T, this->m_vdInstanceId);
        this->CreateCombinedVertexDeclaration(this->m_vdXYZNT1T, this->m_vdInstanceId);

        this->m_vbXyz      = this->AddVb(VERTEX_XYZ, 10922, "GlobalStreaming", 512);
        this->m_vbXyzc     = this->AddVb(VERTEX_XYZC, 0x2000, "GlobalStreaming", 512);
        this->m_vbXyznc    = this->AddVb(VERTEX_XYZNC, 4681, "GlobalStreaming", 512);
        this->m_vbXyzct1   = this->AddVb(VERTEX_XYZCT1, 5461, "GlobalStreaming", 512);
        this->m_vbXyznt1   = this->AddVb(VERTEX_XYZNT1, 4096, "GlobalStreaming", 512);
        this->m_vbXyznt2   = this->AddVb(VERTEX_XYZNT2, 3276, "GlobalStreaming", 512);
        this->m_vbXyznt3   = this->AddVb(VERTEX_XYZNT3, 2730, "GlobalStreaming", 512);
        this->m_vbXyznct2  = this->AddVb(VERTEX_XYZNCT2, 2978, "GlobalStreaming", 512);
        this->m_vbXyznct1  = this->AddVb(VERTEX_XYZNCT1, 3640, "GlobalStreaming", 512);
        this->m_vbXyznt1t  = this->AddVb(VERTEX_XYZNT1T, 2730, "GlobalStreaming", 512);
        this->m_vbXyznct1t = this->AddVb(VERTEX_XYZNCT1T, 2520, "GlobalStreaming", 512);
        this->m_vbXyzwct1  = this->AddVb(VERTEX_XYZWCT1, 4681, "GlobalStreaming", 512);
    };

    void CDevice::DoneVertexDeclarations() {
        this->ReleaseVb(this->m_vbXyz);
        this->ReleaseVb(this->m_vbXyzc);
        this->ReleaseVb(this->m_vbXyznc);
        this->ReleaseVb(this->m_vbXyzct1);
        this->ReleaseVb(this->m_vbXyznt1);
        this->ReleaseVb(this->m_vbXyznt2);
        this->ReleaseVb(this->m_vbXyznt3);
        this->ReleaseVb(this->m_vbXyznct2);
        this->ReleaseVb(this->m_vbXyznct1);
        this->ReleaseVb(this->m_vbXyznt1t);
        this->ReleaseVb(this->m_vbXyznct1t);
        this->ReleaseVb(this->m_vbXyzwct1);

        if (this->m_vdXYZCT1) {
            this->m_vdXYZCT1->Release();
            this->m_vdXYZCT1 = nullptr;
        }
        if (this->m_vdXYZT1) {
            this->m_vdXYZT1->Release();
            this->m_vdXYZT1 = nullptr;
        }
        if (this->m_vdXYZNT1) {
            this->m_vdXYZNT1->Release();
            this->m_vdXYZNT1 = nullptr;
        }
        if (this->m_vdXYZNT2) {
            this->m_vdXYZNT2->Release();
            this->m_vdXYZNT2 = nullptr;
        }
        if (this->m_vdXYZNT3) {
            this->m_vdXYZNT3->Release();
            this->m_vdXYZNT3 = nullptr;
        }
        if (this->m_vdXYZN) {
            this->m_vdXYZN->Release();
            this->m_vdXYZN = nullptr;
        }
        if (this->m_vdXYZ) {
            this->m_vdXYZ->Release();
            this->m_vdXYZ = nullptr;
        }
        if (this->m_vdXYZW) {
            this->m_vdXYZW->Release();
            this->m_vdXYZW = nullptr;
        }
        if (this->m_vdXYZC) {
            this->m_vdXYZC->Release();
            this->m_vdXYZC = nullptr;
        }
        if (this->m_vdXYZNC) {
            this->m_vdXYZNC->Release();
            this->m_vdXYZNC = nullptr;
        }
        if (this->m_vdXYZWCT1) {
            this->m_vdXYZWCT1->Release();
            this->m_vdXYZWCT1 = nullptr;
        }
        if (this->m_vdXYZWC) {
            this->m_vdXYZWC->Release();
            this->m_vdXYZWC = nullptr;
        }
        if (this->m_vdXYZNCT1) {
            this->m_vdXYZNCT1->Release();
            this->m_vdXYZNCT1 = nullptr;
        }
        if (this->m_vdXYZNCT2) {
            this->m_vdXYZNCT2->Release();
            this->m_vdXYZNCT2 = nullptr;
        }
        if (this->m_vdXYZCT2) {
            this->m_vdXYZCT2->Release();
            this->m_vdXYZCT2 = nullptr;
        }
        if (this->m_vdXYZNCT1T) {
            this->m_vdXYZNCT1T->Release();
            this->m_vdXYZNCT1T = nullptr;
        }
        if (this->m_vdXYZNT1T) {
            this->m_vdXYZNT1T->Release();
            this->m_vdXYZNT1T = nullptr;
        }
        if (this->m_vdXYZCT1_UVW) {
            this->m_vdXYZCT1_UVW->Release();
            this->m_vdXYZCT1_UVW = nullptr;
        }
        if (this->m_vdXYZCT2_UVW) {
            this->m_vdXYZCT2_UVW->Release();
            this->m_vdXYZCT2_UVW = nullptr;
        }
        if (this->m_vdXYZNCT1_UV2_S1) {
            this->m_vdXYZNCT1_UV2_S1->Release();
            this->m_vdXYZNCT1_UV2_S1 = nullptr;
        }
        if (this->m_vdWaterTest) {
            this->m_vdWaterTest->Release();
            this->m_vdWaterTest = nullptr;
        }
        if (this->m_vdGrassTest) {
            this->m_vdGrassTest->Release();
            this->m_vdGrassTest = nullptr;
        }
        if (this->m_vdImpostorTest) {
            this->m_vdImpostorTest->Release();
            this->m_vdImpostorTest = nullptr;
        }
        if (this->m_vdYNI) {
            this->m_vdYNI->Release();
            this->m_vdYNI = nullptr;
        }
        if (this->m_vdXYZT1I) {
            this->m_vdXYZT1I->Release();
            this->m_vdXYZT1I = nullptr;
        }
        if (this->m_vdInstanceId) {
            this->m_vdInstanceId->Release();
            this->m_vdInstanceId = nullptr;
        }
    };

    void CDevice::CreateCombinedVertexDeclaration(IDirect3DVertexDeclaration9* a, IDirect3DVertexDeclaration9* b) {
        D3DVERTEXELEMENT9 aElems[64];
        D3DVERTEXELEMENT9 bElems[64];
        uint32_t aNumElems, bNumElems;

        if (FAILED(a->GetDeclaration(aElems, &aNumElems)))
            LOG_ERROR("CreateCombinedVertexDeclaration failed.");
        if (FAILED(b->GetDeclaration(bElems, &bNumElems)))
            LOG_ERROR("CreateCombinedVertexDeclaration failed.");

        std::vector<D3DVERTEXELEMENT9> combined;

        // Append first decl (skip D3DDECL_END)
        for (uint32_t i = 0; i < aNumElems - 1; i++)
            combined.push_back(aElems[i]);

        // Append second decl (skip D3DDECL_END)
        for (uint32_t i = 0; i < bNumElems - 1; i++)
            combined.push_back(bElems[i]);

        // Add D3DDECL_END
        combined.push_back(D3DDECL_END());

        IDirect3DVertexDeclaration9* combinedVD;
        this->m_pd3dDevice->CreateVertexDeclaration(combined.data(), &combinedVD);

        // Cache in map: {a, b} -> combinedVD
        this->m_combinedVD[{a, b}] = combinedVD;
    };

    void CDevice::ActuateStates(bool bForFFP) {
        if (m_userClipPlaneEnabled)
            this->ActuateClipPlanes(bForFFP);

        if (bForFFP)
            this->ActuateMatrices();

        if (this->m_latchedCheck) {
            this->m_latchedCheck = false;
            if (this->m_latchedIb != m_curIb) {
                this->m_curIb = m_latchedIb;
                _SetLastResult(m_pd3dDevice->SetIndices(m_latchedIb));
            }
        }
    };

    void CDevice::ActuateMatrices() {
        if (this->m_matWorldIsNotActuated) {
            this->m_pd3dDevice->SetTransform(D3DTS_WORLD, (D3DXMATRIX*)&this->m_matWorldStack[m_matWorldStackTop]);
            this->m_matWorldIsNotActuated = false;
            this->m_stats.swMatrices++;
        }

        if (this->m_matViewIsNotActuated) {
            this->m_pd3dDevice->SetTransform(D3DTS_VIEW, (D3DXMATRIX*)&this->m_matViewStack[m_matViewStackTop]);
            this->m_matViewIsNotActuated = false;
            this->m_stats.swMatrices++;
        }
    };

    template <typename T> inline void _ResetState(CDevice* self, D3DRENDERSTATETYPE key, T val) {
        if (self->m_curRenderState[key] != val) {
            self->m_curRenderState[key] = val;
            self->m_pd3dDevice->SetRenderState(key, val);
            self->m_stats.swRenderStates++;
        }
    };

    template <typename T> inline void _ResetStage(CDevice* self, size_t idx, D3DTEXTURESTAGESTATETYPE key, T val) {
        if (self->m_curTexStagesStates[idx][key] != val) {
            self->m_curTexStagesStates[idx][key] = val;
            self->m_pd3dDevice->SetTextureStageState(idx, key, val);
            self->m_stats.swTextureStageStates++;
        }
    }

    template <typename T> inline void _ResetSampler(CDevice* self, size_t idx, D3DSAMPLERSTATETYPE key, T val) {
        if (self->m_curTexSamplerStates[idx][key] != val) {
            self->m_curTexSamplerStates[idx][key] = val;
            self->m_pd3dDevice->SetSamplerState(idx, key, val);
            self->m_stats.swTextureSamplerStates++;
        }
    }

    void CDevice::internalReset() {
        rstCaches();

        memset(this->m_curRenderState, 0xFFu, sizeof(this->m_curRenderState));
        _ResetState(this, D3DRS_ZENABLE, 1);
        _ResetState(this, D3DRS_FILLMODE, 3);
        _ResetState(this, D3DRS_SHADEMODE, 2);
        _ResetState(this, D3DRS_ZWRITEENABLE, 1);
        _ResetState(this, D3DRS_ALPHATESTENABLE, 0);
        _ResetState(this, D3DRS_LASTPIXEL, 1);
        _ResetState(this, D3DRS_SRCBLEND, 2);
        _ResetState(this, D3DRS_DESTBLEND, 1);
        _ResetState(this, D3DRS_CULLMODE, 3);
        _ResetState(this, D3DRS_ZFUNC, 4);
        _ResetState(this, D3DRS_ALPHAREF, 0);
        _ResetState(this, D3DRS_ALPHAFUNC, 8);
        _ResetState(this, D3DRS_DITHERENABLE, 0);
        _ResetState(this, D3DRS_ALPHABLENDENABLE, 0);
        _ResetState(this, D3DRS_FOGENABLE, 0);
        _ResetState(this, D3DRS_SPECULARENABLE, 0);
        _ResetState(this, D3DRS_FOGCOLOR, 0);
        _ResetState(this, D3DRS_FOGTABLEMODE, 0);
        _ResetState(this, D3DRS_FOGSTART, 0);
        _ResetState(this, D3DRS_FOGEND, 0);
        _ResetState(this, D3DRS_FOGDENSITY, 1.0f);
        _ResetState(this, D3DRS_ANTIALIASEDLINEENABLE, 0);
        _ResetState(this, D3DRS_RANGEFOGENABLE, 0);
        _ResetState(this, D3DRS_STENCILENABLE, 0);
        _ResetState(this, D3DRS_STENCILFAIL, 1);
        _ResetState(this, D3DRS_STENCILZFAIL, 1);
        _ResetState(this, D3DRS_STENCILPASS, 1);
        _ResetState(this, D3DRS_STENCILFUNC, 8);
        _ResetState(this, D3DRS_STENCILREF, 0);
        _ResetState(this, D3DRS_STENCILMASK, -1);
        _ResetState(this, D3DRS_STENCILWRITEMASK, -1);
        _ResetState(this, D3DRS_TEXTUREFACTOR, 0);
        _ResetState(this, D3DRS_WRAP0, 0);
        _ResetState(this, D3DRS_WRAP1, 0);
        _ResetState(this, D3DRS_WRAP2, 0);
        _ResetState(this, D3DRS_WRAP3, 0);
        _ResetState(this, D3DRS_WRAP4, 0);
        _ResetState(this, D3DRS_WRAP5, 0);
        _ResetState(this, D3DRS_WRAP6, 0);
        _ResetState(this, D3DRS_WRAP7, 0);
        _ResetState(this, D3DRS_CLIPPING, 1);
        _ResetState(this, D3DRS_LIGHTING, 1);
        _ResetState(this, D3DRS_AMBIENT, 0);
        _ResetState(this, D3DRS_FOGVERTEXMODE, 0);
        _ResetState(this, D3DRS_COLORVERTEX, 1);
        _ResetState(this, D3DRS_LOCALVIEWER, 1);
        _ResetState(this, D3DRS_NORMALIZENORMALS, 1);
        _ResetState(this, D3DRS_DIFFUSEMATERIALSOURCE, 1);
        _ResetState(this, D3DRS_SPECULARMATERIALSOURCE, 0);
        _ResetState(this, D3DRS_AMBIENTMATERIALSOURCE, 0);
        _ResetState(this, D3DRS_VERTEXBLEND, 0);
        _ResetState(this, D3DRS_CLIPPLANEENABLE, 0);
        _ResetState(this, D3DRS_POINTSIZE, 1.0f);
        _ResetState(this, D3DRS_POINTSIZE_MIN, 1.0f);
        _ResetState(this, D3DRS_POINTSCALEENABLE, 0);
        _ResetState(this, D3DRS_POINTSCALE_A, 1.0f);
        _ResetState(this, D3DRS_POINTSCALE_B, 1.0f);
        _ResetState(this, D3DRS_POINTSCALE_C, 1.0f);
        _ResetState(this, D3DRS_MULTISAMPLEANTIALIAS, 0);
        _ResetState(this, D3DRS_MULTISAMPLEMASK, -1);
        _ResetState(this, D3DRS_PATCHEDGESTYLE, 0);
        _ResetState(this, D3DRS_DEBUGMONITORTOKEN, 0);
        _ResetState(this, D3DRS_POINTSIZE_MAX, 1.0f);
        _ResetState(this, D3DRS_INDEXEDVERTEXBLENDENABLE, 0);
        _ResetState(this, D3DRS_TWEENFACTOR, 0);
        _ResetState(this, D3DRS_BLENDOP, 1);
        _ResetState(this, D3DRS_POSITIONDEGREE, 3);
        _ResetState(this, D3DRS_NORMALDEGREE, 1);
        _ResetState(this, D3DRS_SCISSORTESTENABLE, 0);
        _ResetState(this, D3DRS_SLOPESCALEDEPTHBIAS, 0);
        _ResetState(this, D3DRS_MINTESSELLATIONLEVEL, 1.0f);
        _ResetState(this, D3DRS_MAXTESSELLATIONLEVEL, 1.0f);
        _ResetState(this, D3DRS_ADAPTIVETESS_X, 0);
        _ResetState(this, D3DRS_ADAPTIVETESS_Y, 0);
        _ResetState(this, D3DRS_ADAPTIVETESS_Z, 1.0f);
        _ResetState(this, D3DRS_ADAPTIVETESS_W, 0);
        _ResetState(this, D3DRS_TWOSIDEDSTENCILMODE, 0);
        _ResetState(this, D3DRS_CCW_STENCILFAIL, 1);
        _ResetState(this, D3DRS_CCW_STENCILZFAIL, 1);
        _ResetState(this, D3DRS_CCW_STENCILPASS, 8);
        _ResetState(this, D3DRS_CCW_STENCILFUNC, 8);
        _ResetState(this, D3DRS_COLORWRITEENABLE1, 15);
        _ResetState(this, D3DRS_COLORWRITEENABLE2, 15);
        _ResetState(this, D3DRS_COLORWRITEENABLE3, 15);
        _ResetState(this, D3DRS_BLENDFACTOR, -1);
        _ResetState(this, D3DRS_SRGBWRITEENABLE, 0);
        _ResetState(this, D3DRS_DEPTHBIAS, 0);
        _ResetState(this, D3DRS_WRAP8, 0);
        _ResetState(this, D3DRS_WRAP9, 0);
        _ResetState(this, D3DRS_WRAP10, 0);
        _ResetState(this, D3DRS_WRAP11, 0);
        _ResetState(this, D3DRS_WRAP12, 0);
        _ResetState(this, D3DRS_WRAP13, 0);
        _ResetState(this, D3DRS_WRAP14, 0);
        _ResetState(this, D3DRS_WRAP15, 0);
        _ResetState(this, D3DRS_SEPARATEALPHABLENDENABLE, 0);
        _ResetState(this, D3DRS_SRCBLENDALPHA, 2);
        _ResetState(this, D3DRS_DESTBLENDALPHA, 1);
        _ResetState(this, D3DRS_BLENDOPALPHA, 1);

        for (size_t idx = 0; idx < 8; idx++) {
            memset(this->m_curTexStagesStates[idx], 0xFF, sizeof(this->m_curTexStagesStates[idx]));
            memset(this->m_curTexSamplerStates[idx], 0xFF, sizeof(this->m_curTexSamplerStates[idx]));

            _ResetStage(this, idx, D3DTSS_COLOROP, idx == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE);
            _ResetStage(this, idx, D3DTSS_COLORARG1, 2);
            _ResetStage(this, idx, D3DTSS_COLORARG2, 1);
            _ResetStage(this, idx, D3DTSS_ALPHAOP, idx == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE);
            _ResetStage(this, idx, D3DTSS_ALPHAARG1, 0);
            _ResetStage(this, idx, D3DTSS_ALPHAARG2, 1);
            _ResetStage(this, idx, D3DTSS_BUMPENVMAT00, 0);
            _ResetStage(this, idx, D3DTSS_BUMPENVMAT01, 0);
            _ResetStage(this, idx, D3DTSS_BUMPENVMAT11, 0);
            _ResetStage(this, idx, D3DTSS_TEXCOORDINDEX, idx);
            _ResetStage(this, idx, D3DTSS_BUMPENVLSCALE, 0);
            _ResetStage(this, idx, D3DTSS_BUMPENVLOFFSET, 0);
            _ResetStage(this, idx, D3DTSS_TEXTURETRANSFORMFLAGS, 0);
            _ResetStage(this, idx, D3DTSS_COLORARG0, 0);
            _ResetStage(this, idx, D3DTSS_ALPHAARG0, 0);
            _ResetStage(this, idx, D3DTSS_RESULTARG, 1);
            _ResetStage(this, idx, D3DTSS_CONSTANT, 0);

            _ResetSampler(this, idx, D3DSAMP_ADDRESSU, 1);
            _ResetSampler(this, idx, D3DSAMP_ADDRESSV, 1);
            _ResetSampler(this, idx, D3DSAMP_ADDRESSW, 1);
            _ResetSampler(this, idx, D3DSAMP_BORDERCOLOR, 0);
            _ResetSampler(this, idx, D3DSAMP_MAGFILTER, 1);
            _ResetSampler(this, idx, D3DSAMP_MINFILTER, 1);
            _ResetSampler(this, idx, D3DSAMP_MIPFILTER, 0);
            _ResetSampler(this, idx, D3DSAMP_MIPMAPLODBIAS, 0);
            _ResetSampler(this, idx, D3DSAMP_MAXMIPLEVEL, 0);
            _ResetSampler(this, idx, D3DSAMP_MAXANISOTROPY, 1);
            _ResetSampler(this, idx, D3DSAMP_SRGBTEXTURE, 0);
            _ResetSampler(this, idx, D3DSAMP_ELEMENTINDEX, 0);
            _ResetSampler(this, idx, D3DSAMP_DMAPOFFSET, 256);
        }
        rstStacks();
    };

    void CDevice::rstCaches() {
        m_curFVF          = 0;
        m_curVertexShader = nullptr;
        m_curPixelShader  = nullptr;
        m_curVertexDecl   = nullptr;
        m_curIb           = nullptr;
        m_latchedIb       = nullptr;

        std::fill_n(m_curVb, 4, nullptr); // count=4, value=nullptr
        std::fill_n(m_curStride, 4, 0u);  // count=4, value=0

        for (size_t idx = 0; idx < 8; idx++) {
            _SetLastResult(m_pd3dDevice->SetTexture(idx, nullptr));
            m_curTexStages[idx] = nullptr;
        }
    }

    void CDevice::rstTexPrepareFor() {
        for (auto [_, image] : this->mActiveImages) {
            if (image->mPool == D3DPOOL_DEFAULT) {
                image->mHandleBase->Release();
                image->mHandleBase = nullptr;
            }
        }
    };

    bool CDevice::rstTexRestoreAfter() {
        uint32_t failCount = 0;

        for (auto [index, image] : this->mActiveImages) {
            if (image->mPool != D3DPOOL_DEFAULT)
                continue;

            HRESULT hr;
            if (image->Is2D()) {
                hr = m_pd3dDevice->CreateTexture(
                    image->mSurface2D.Width,
                    image->mSurface2D.Height,
                    1,
                    image->mUsage,
                    image->mSurface2D.Format,
                    image->mPool,
                    &image->mHandle2D,
                    nullptr
                );
            } else if (image->IsCube()) {
                hr = m_pd3dDevice->CreateCubeTexture(
                    image->mSurface2D.Width, 1, image->mUsage, image->mSurface2D.Format, image->mPool, &image->mHandleCube, nullptr
                );
            } else if (image->Is3D()) {
                hr = m_pd3dDevice->CreateVolumeTexture(
                    image->mSurface3D.Width,
                    image->mSurface3D.Height,
                    image->mSurface3D.Depth,
                    1,
                    image->mUsage,
                    image->mSurface3D.Format,
                    image->mPool,
                    &image->mHandle3D,
                    nullptr
                );
            }

            if (FAILED(hr)) {
                LOG_ERROR("cannot restore texmap %u", index);
                image->mHandleBase = nullptr;
                failCount++;
            }
        }

        if (failCount > 0) {
            LOG_ERROR("TexMan failed to restore %u textures", failCount);
            return false;
        }

        return true;
    };

    void CDevice::rstIbPrepareFor() {
        for (auto& ib : m_ibs) {
            if (ib.m_ib && ib.m_desc.Pool == D3DPOOL_DEFAULT) {
                ib.m_ib->Release();
                ib.m_curPos = 0;
                ib.m_locked = false;
            }
        }
    };

    void CDevice::rstIbRestoreAfter() {
        uint32_t failCount = 0;

        for (auto& ib : m_ibs) {
            if (!ib.m_ib || ib.m_desc.Pool != D3DPOOL_DEFAULT)
                continue;

            HRESULT hr =
                m_pd3dDevice->CreateIndexBuffer(ib.m_desc.Size, ib.m_desc.Usage, ib.m_desc.Format, ib.m_desc.Pool, &ib.m_ib, nullptr);

            if (FAILED(hr)) {
                ib.m_ib = nullptr;
                failCount++;
            }
        }

        if (failCount > 0)
            LOG_ERROR("IBMan failed to restore %u ibs", failCount);
    };

    void CDevice::rstVbPrepareFor() {
        for (auto& vb : m_vbs) {
            if (vb.m_vb && vb.m_desc.Pool == D3DPOOL_DEFAULT) {
                vb.m_vb->Release();
                vb.m_curPos = 0;
                vb.m_locked = false;
            }
        }
    };

    void CDevice::rstVbRestoreAfter() {
        uint32_t failCount = 0;

        for (auto& vb : m_vbs) {
            if (!vb.m_vb || vb.m_desc.Pool != D3DPOOL_DEFAULT)
                continue;

            HRESULT hr =
                m_pd3dDevice->CreateVertexBuffer(vb.m_desc.Size, vb.m_desc.Usage, vb.m_desc.FVF, vb.m_desc.Pool, &vb.m_vb, nullptr);

            if (FAILED(hr)) {
                vb.m_vb = nullptr;
                failCount++;
            }
        }

        if (failCount > 0)
            LOG_ERROR("VBMan failed to restore %u vbs", failCount);
    };

    void CDevice::GetVertexInfo(VertexType type, uint32_t& fvf, int32_t& stride, IDirect3DVertexDeclaration9*& decl) {
        struct VertexInfo {
            uint32_t fvf;
            int32_t stride;
            IDirect3DVertexDeclaration9* decl;
        };

        const VertexInfo info[0x20] = {
            {0, 12, m_vdXYZ},           // 0: VERTEX_XYZ - Pos(12)
            {0, 20, m_vdXYZT1},         // 1: VERTEX_XYZT1 - Pos(12) + UV(8)
            {0, 16, m_vdXYZC},          // 2: VERTEX_XYZC - Pos(12) + Color(4)
            {0, 20, m_vdXYZWC},         // 3: VERTEX_XYZWC - PosT(16) + Color(4)
            {0, 28, m_vdXYZWCT1},       // 4: VERTEX_XYZWCT1 - PosT(16) + Color(4) + UV(8)
            {0, 28, m_vdXYZNC},         // 5: VERTEX_XYZNC - Pos(12) + Norm(12) + Color(4)
            {0, 24, m_vdXYZCT1},        // 6: VERTEX_XYZCT1 - Pos(12) + Color(4) + UV(8)
            {0, 32, m_vdXYZNT1},        // 7: VERTEX_XYZNT1 - Pos(12) + Norm(12) + UV(8)
            {0, 36, m_vdXYZNCT1},       // 8: VERTEX_XYZNCT1 - Pos(12) + Norm(12) + Color(4) + UV(8)
            {0, 44, m_vdXYZNCT2},       // 9: VERTEX_XYZNCT2 - Pos(12) + Norm(12) + Color(4) + UV(8) + UV2(8)
            {0, 40, m_vdXYZNT2},        // 10: VERTEX_XYZNT2 - Pos(12) + Norm(12) + UV(8) + UV2(8)
            {0, 48, m_vdXYZNT3},        // 11: VERTEX_XYZNT3 - Pos(12) + Norm(12) + UV(8) + UV2(8) + UV3(8)
            {0, 28, m_vdXYZCT1_UVW},    // 12: VERTEX_XYZCT1_UVW - Pos(12) + Color(4) + UVW(12)
            {0, 36, m_vdXYZCT2_UVW},    // 13: VERTEX_XYZCT2_UVW - Pos(12) + Color(4) + UVW(12) + UV2(8)
            {0, 32, m_vdXYZCT2},        // 14: VERTEX_XYZCT2 - Pos(12) + Color(4) + UV(8) + UV2(8)
            {0, 48, m_vdXYZNT1T},       // 15: VERTEX_XYZNT1T - Pos(12) + Norm(12) + Tan(16) + UV(8)
            {0, 52, m_vdXYZNCT1T},      // 16: VERTEX_XYZNCT1T - Pos(12) + Norm(12) + Color(4) + UV(8) + Tan(16)
            {0, 8, m_vdXYZNCT1_UV2_S1}, // 17: VERTEX_XYZNCT1_UV2_S1 - stream 0: Y(4) + NI(4)
            {0, 8, nullptr},            // 18: VERTEX_STREAM_UV_S1 - stream 1: UV(8)
            {0, 8, m_vdWaterTest},      // 19: VERTEX_WATERTEST - Short4(8)
            {0, 24, m_vdGrassTest},     // 20: VERTEX_GRASSTEST - Pos4(16) + UV(8)
            {0, 20, m_vdImpostorTest},  // 21: VERTEX_IMPOSTORTEST - Pos(12) + UV(8)
            {0, 12, m_vdYNI},           // 22: VERTEX_YNI - Y(4) + Short4(8)
            {0, 24, m_vdXYZT1I},        // 23: VERTEX_XYZT1I - Pos(12) + UV(8) + Short2(4)
            {0, 4, m_vdInstanceId},     // 24: INSTANCE_ID - stream 1: Short2(4)
        };

        if (type >= 0 && type < ARRAYSIZE(info)) {
            fvf    = info[type].fvf;
            stride = info[type].stride;
            decl   = info[type].decl;
        } else {
            fvf    = 0;
            stride = 0;
            decl   = nullptr;
        }
    };

    HRESULT CDevice::setTexture(int32_t stage, Texture* tex) {
        m_stats.swTextures++;
        return m_pd3dDevice->SetTexture(stage, *tex);
    };

    void CDevice::setGamma(float, float, float) {};

    TexHandle CDevice::readShader(const hta::CStr& name, unsigned int flags) {
        TexHandle result;
        result.m_handle = -1;

        // Verify file signature
        {
            hta::m3d::fs::FileStream* stream = hta::m3d::Kernel::Instance()->GetFileServer().CreateFileStream();

            if (!stream->Open(name.m_charPtr, hta::m3d::fs::IStream::OPEN_READ)) {
                delete stream;
                return result;
            }

            char signature[20] = {0};
            stream->ReadBytes(signature, 19);
            stream->Close();
            delete stream;

            if (strcmp(signature, "<!-- M3D_SHADER -->") != 0)
                return result;
        }

        // Parse shader INI file
        ref_ptr<hta::m3d::cmn::IniFile> parser(hta::m3d::Kernel::Instance()->CreateIniFile());

        {
            hta::m3d::fs::FileStream* stream = hta::m3d::Kernel::Instance()->GetFileServer().CreateFileStream();

            if (!stream->Open(name.m_charPtr, hta::m3d::fs::IStream::OPEN_READ)) {
                delete stream;
                return result;
            }

            parser->Read(*stream);
            stream->Close();
            delete stream;

            // Check for parse errors
            const char* error = parser->GetError();
            if (error) {
                LOG_INFO("parse error: file: %s err: %s", name.m_charPtr, error);
                return result;
            }
        }

        // Read shader properties
        int fps                = parser->GetInteger("MAPS", "FPS");
        hta::CStr framePattern = parser->GetString("MAPS", "FRAME");
        bool looped            = (parser->GetInteger("MAPS", "LOOPED") != 0);
        bool pingPong          = (parser->GetInteger("MAPS", "LOOPED") == 2);

        if (framePattern.empty()) {
            LOG_INFO("shader: %s tex frames are not specified", name.m_charPtr);
            return result;
        }

        // Create texture and load frames
        Sampler* texture = this->CreateTexture(result.m_handle);

        if (!loadTexMaps(texture, framePattern, flags)) {
            this->DeleteTexture(result.m_handle);
            return result;
        }

        // If ping-pong mode, duplicate frames in reverse order
        if (pingPong) {
            int numFrames = texture->m_maps.size();
            texture->m_maps.reserve(numFrames * 2 - 1);
            for (int i = numFrames - 2; i >= 0; --i) {
                Texture* map = texture->m_maps[i];
                texture->m_maps.push_back(map);
                map->AddRef();
            }
        }

        // Initialize texture properties
        texture->m_address[0]    = D3DTADDRESS_WRAP;
        texture->m_address[1]    = D3DTADDRESS_WRAP;
        texture->m_address[2]    = D3DTADDRESS_WRAP;
        texture->m_maxAnisotropy = 1;
        texture->m_mipFilter     = D3DTEXF_POINT;
        texture->m_magFilter     = D3DTEXF_LINEAR;
        texture->m_minFilter     = D3DTEXF_LINEAR;
        texture->m_borderColor   = 0;
        texture->m_lodBias       = 0.0f;
        texture->m_lodMax        = 0;
        texture->m_looped        = looped;
        texture->m_fps           = fps;
        texture->mRefs           = 0;
        texture->mFileName       = name;

        texture->AddRef();

        return result;
    };

    Sampler* CDevice::CreateTexture(int32_t& handle) {
        handle       = this->mActiveTextures.GetSlot();
        Sampler* tex = new Sampler();
        this->mActiveTextures.AddItem(handle, tex);
        return tex;
    };

    void CDevice::DeleteTexture(int32_t handle) {
        Sampler* texture = mActiveTextures.GetItem(handle);
        if (!texture)
            return;

        // Release all maps
        for (auto& map : texture->m_maps) {
            if (map->GetRef() == 1) {
                for (auto& [slot, img] : mActiveImages) {
                    if (img == map) {
                        mActiveImages.DelItem(slot);
                        break;
                    }
                }
                map->DecRef();
            }
        }

        texture->m_maps.clear();
        mActiveTextures.DelItem(handle);
        delete texture;
    }

    int32_t CDevice::loadTexMaps(Sampler* tex, const hta::CStr& pattern, uint32_t flags) {
        hta::CStr filename = pattern;

        char* percentPos = strchr(filename.m_charPtr, '%');
        if (!percentPos) {
            Texture* map = addTexMap(filename, flags);
            if (!map)
                return false;
            tex->m_maps.push_back(map);
            return true;
        }

        int patternIdx = percentPos - filename.m_charPtr;
        int numDigits  = 0;

        while (percentPos[numDigits] == '%' && numDigits < 3)
            numDigits++;

        int maxFrames = 1;
        for (int i = 0; i < numDigits; i++)
            maxFrames *= 10;

        char buffer[4];
        for (int frame = 0; frame < maxFrames; frame++) {
            if (numDigits == 1)
                sprintf(buffer, "%.1d", frame);
            else if (numDigits == 2)
                sprintf(buffer, "%.2d", frame);
            else
                sprintf(buffer, "%.3d", frame);

            memcpy(&filename.m_charPtr[patternIdx], buffer, numDigits);

            auto& fs    = hta::m3d::Kernel::Instance()->GetFileServer();
            auto stream = fs.CreateFileStream();
            if (!stream->Open(filename.m_charPtr, hta::m3d::fs::IStream::OPEN_READ))
                break;
            stream->Close();

            // Load texture
            Texture* map = addTexMap(filename, flags);
            if (!map)
                break;

            tex->m_maps.push_back(map);
        }

        return !tex->m_maps.empty();
    };

    void CDevice::UpdateVBMemStats() {
        std::vector<CVertexBuffer*> vbPools;
        for (auto& pool : m_VbPoolBuffers) {
            for (auto& handle : pool) {
                vbPools.push_back(&m_vbs[handle.m_handle]);
            }
        }

        m_devMemStats.DynamicVBCount = 0;
        m_devMemStats.DynamicVBSize  = 0;
        m_devMemStats.StaticVBCount  = 0;
        m_devMemStats.StaticVBSize   = 0;
        m_devMemStats.VBPoolsSize    = 0;

        for (auto& vb : m_vbs) {
            auto it = std::find(vbPools.begin(), vbPools.end(), &vb);

            if (it != vbPools.end()) {
                m_devMemStats.VBPoolsSize += vb.m_desc.Size;
            } else if (vb.m_desc.Usage & D3DUSAGE_DYNAMIC) {
                m_devMemStats.DynamicVBSize += vb.m_desc.Size;
                m_devMemStats.DynamicVBCount++;
            } else {
                m_devMemStats.StaticVBSize += vb.m_desc.Size;
                m_devMemStats.StaticVBCount++;
            }
        }
    };

    void CDevice::UpdateIBMemStats() {
        std::vector<CIndexBuffer*> ibPools;
        for (auto& handle : m_IbPoolBuffers) {
            ibPools.push_back(&m_ibs[handle.m_handle]);
        }

        m_devMemStats.DynamicIBCount = 0;
        m_devMemStats.DynamicIBSize  = 0;
        m_devMemStats.StaticIBCount  = 0;
        m_devMemStats.StaticIBSize   = 0;
        m_devMemStats.IBPoolsSize    = 0;

        for (auto& ib : m_ibs) {
            auto it = std::find(ibPools.begin(), ibPools.end(), &ib);

            if (it != ibPools.end()) {
                m_devMemStats.IBPoolsSize += ib.m_desc.Size;
            } else if (ib.m_desc.Usage & D3DUSAGE_DYNAMIC) {
                m_devMemStats.DynamicIBSize += ib.m_desc.Size;
                m_devMemStats.DynamicIBCount++;
            } else {
                m_devMemStats.StaticIBSize += ib.m_desc.Size;
                m_devMemStats.StaticIBCount++;
            }
        }
    };

    void CDevice::UpdateTexMemStats() {
        throw std::runtime_error("not implemented");
    };

    D3DFORMAT fmtFullscreenArray[8]{
        D3DFMT_R5G6B5,
        D3DFMT_X1R5G5B5,
        D3DFMT_A1R5G5B5,
        D3DFMT_X4R4G4B4,
        D3DFMT_A4R4G4B4,
        D3DFMT_R8G8B8,
        D3DFMT_A8R8G8B8,
        D3DFMT_X8R8G8B8,
    };

    const int bits[8]{
        0x10,
        0x10,
        0x10,
        0x10,
        0x10,
        0x20,
        0x20,
        0x20,
    };

    D3DFORMAT tfRgbaFormats[6]{
        D3DFMT_A1R5G5B5,
        D3DFMT_A4R4G4B4,
        D3DFMT_A8R8G8B8,
        D3DFMT_DXT5,
        D3DFMT_DXT3,
        D3DFMT_DXT1,
    };

    const int tfRgbaBits[6]{
        0x10,
        0x10,
        0x20,
        0,
        0,
        0,
    };

    const D3DFORMAT tfRgbFormats[3]{
        D3DFMT_R8G8B8,
        D3DFMT_R5G6B5,
        D3DFMT_DXT1,
    };

    const int tfRgbBits[3]{
        0x18,
        0x10,
        0,
    };

    const _D3DFORMAT tfRgbaFormatsRts[7]{
        D3DFMT_R5G6B5,
        D3DFMT_A1R5G5B5,
        D3DFMT_A4R4G4B4,
        D3DFMT_A8R8G8B8,
        D3DFMT_X1R5G5B5,
        D3DFMT_X4R4G4B4,
        D3DFMT_X8R8G8B8,
    };

    const int tfRgbaBitsRts[7]{
        0x10,
        0x10,
        0x10,
        0x20,
        0x10,
        0x10,
        0x20,
    };

    const D3DFORMAT tfDepthFormats[6]{
        D3DFMT_D24S8,
        D3DFMT_D24X4S4,
        D3DFMT_D24X8,
        D3DFMT_D32,
        D3DFMT_D16,
        D3DFMT_D15S1,
    };

    const int tfDepthBits[6]{
        0x20,
        0x20,
        0x20,
        0x20,
        0x10,
        0x10,
    };

    static const int fmtDepthArrayBits[6]{
        // Add static, make const
        0x20,
        0x10,
        0x20,
        0x20,
        0x10,
        0x20,
    };

    static const D3DFORMAT fmtDepthArray[6]{
        // Add const
        D3DFMT_D24S8,
        D3DFMT_D15S1,
        D3DFMT_D24X4S4,
        D3DFMT_D24X8,
        D3DFMT_D16,
        D3DFMT_D32,
    };

    int32_t CDevice::FindDepthFormat(D3DFORMAT targetFormat, D3DFORMAT* pDepthStencilFormat, int32_t bpp) {
        // Try first set of depth formats (count=3)
        if (FindDepthFormat(*pDepthStencilFormat, targetFormat, fmtDepthArray, fmtDepthArrayBits, 3, bpp)) {
            return 1;
        }

        // Try second set of depth formats (count=6)
        if (FindDepthFormat(*pDepthStencilFormat, targetFormat, fmtDepthArray, fmtDepthArrayBits, 6, bpp)) {
            return 1;
        }

        return 0;
    }

    int32_t CDevice::FindDepthFormat(
        D3DFORMAT& depthFormat,
        D3DFORMAT targetFormat,
        const D3DFORMAT* fmtDepthArray, // Add const
        const int* fmtDepthArrayBits,   // Add const
        int numDepthFmts,
        int bpp
    ) {
        // First pass: try to match exact bit depth if specified
        if (bpp > 0) {
            for (int i = 0; i < numDepthFmts; ++i) {
                // Check if hardware supports this depth format
                this->m_lastResult = m_pD3D->CheckDeviceFormat(
                    D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, targetFormat, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, fmtDepthArray[i]
                );

                if (FAILED(m_lastResult))
                    continue;

                // Check if bit depth matches
                if (fmtDepthArrayBits[i] != bpp)
                    continue;

                // Check if this depth format works with the target format
                this->m_lastResult =
                    m_pD3D->CheckDepthStencilMatch(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, targetFormat, targetFormat, fmtDepthArray[i]);

                if (SUCCEEDED(m_lastResult)) {
                    depthFormat = fmtDepthArray[i];
                    return 1;
                }
            }
        }

        // Second pass: try any compatible format (ignore bit depth)
        for (int i = 0; i < numDepthFmts; ++i) {
            // Check if hardware supports this depth format
            this->m_lastResult = m_pD3D->CheckDeviceFormat(
                D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, targetFormat, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, fmtDepthArray[i]
            );

            if (FAILED(m_lastResult))
                continue;

            // Check if this depth format works with the target format
            this->m_lastResult =
                m_pD3D->CheckDepthStencilMatch(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, targetFormat, targetFormat, fmtDepthArray[i]);

            if (SUCCEEDED(m_lastResult)) {
                depthFormat = fmtDepthArray[i];
                return 1;
            }
        }

        return 0; // No compatible depth format found
    }

    int32_t CDevice::FindTexFormat(
        D3DFORMAT& texFormat,
        D3DFORMAT TargetFormat,
        const D3DFORMAT* fmtTextureArray,
        const int32_t* bits,
        int32_t numTextureFmts,
        int32_t bpp,
        int32_t usage
    ) {
        for (int32_t iFmt = 0; iFmt < numTextureFmts; ++iFmt) {
            D3DFORMAT format  = fmtTextureArray[iFmt];
            int32_t formatBpp = bits[iFmt];

            if (formatBpp != bpp)
                continue;

            HRESULT hr = m_pD3D->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, TargetFormat, usage, D3DRTYPE_TEXTURE, format);

            this->m_lastResult = hr;

            if (SUCCEEDED(hr)) {
                texFormat = format;
                return 1;
            }
        }
        return 0;
    };

    int32_t CDevice::FindSurfaceFormat(D3DFORMAT* pSurfaceFormat, int32_t bpp) {
        constexpr int32_t NUM_FORMATS = 8;

        for (int32_t i = 0; i < NUM_FORMATS; ++i) {
            D3DFORMAT format  = fmtFullscreenArray[i];
            int32_t formatBpp = bits[i];

            if (formatBpp != bpp)
                continue;

            HRESULT hr = m_pD3D->CheckDeviceType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, format, format, FALSE);

            this->m_lastResult = hr;

            if (SUCCEEDED(hr)) {
                *pSurfaceFormat = format;
                return 1;
            }
        }

        for (int32_t i = 0; i < NUM_FORMATS; ++i) {
            D3DFORMAT format = fmtFullscreenArray[i];

            HRESULT hr = m_pD3D->CheckDeviceType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, format, format, FALSE);

            this->m_lastResult = hr;

            if (SUCCEEDED(hr)) {
                *pSurfaceFormat = format;
                return 1;
            }
        }

        return 0;
    };

    int32_t CDevice::FindTextureFormat(D3DFORMAT TargetFormat, int32_t bpp) {
        if (!FindTexFormat(m_texFormat[0][0], TargetFormat, tfRgbaFormats, tfRgbaBits, 6, bpp, 0) &&
            !FindTexFormat(m_texFormat[0][0], TargetFormat, tfRgbaFormats, tfRgbaBits, 6, 0, 0) &&
            !FindTexFormat(m_texFormat[0][0], TargetFormat, tfRgbaFormats, tfRgbaBits, 6, 16, 0) &&
            !FindTexFormat(m_texFormat[0][0], TargetFormat, tfRgbaFormats, tfRgbaBits, 6, 32, 0)) {
            return 0;
        }
        if (!FindTexFormat(m_texFormat[0][1], TargetFormat, tfRgbaFormats, tfRgbaBits, 6, 32, 0) &&
            !FindTexFormat(m_texFormat[0][1], TargetFormat, tfRgbaFormats, tfRgbaBits, 6, 16, 0)) {
            return 0;
        }
        if (!FindTexFormat(m_texFormat[1][0], TargetFormat, tfRgbFormats, tfRgbBits, 3, 0, 0) &&
            !FindTexFormat(m_texFormat[1][0], TargetFormat, tfRgbFormats, tfRgbBits, 3, 16, 0) &&
            !FindTexFormat(m_texFormat[1][0], TargetFormat, tfRgbFormats, tfRgbBits, 3, 24, 0)) {
            return 0;
        }
        if (!FindTexFormat(m_texFormat[1][1], TargetFormat, tfRgbFormats, tfRgbBits, 3, 24, 0) &&
            !FindTexFormat(m_texFormat[1][1], TargetFormat, tfRgbFormats, tfRgbBits, 3, 16, 0)) {
            return 0;
        }
        if (!FindTexFormat(m_texFormatRt, TargetFormat, tfRgbaFormatsRts, tfRgbaBitsRts, 7, 32, D3DUSAGE_RENDERTARGET) &&
            !FindTexFormat(m_texFormatRt, TargetFormat, tfRgbaFormatsRts, tfRgbaBitsRts, 7, 16, D3DUSAGE_RENDERTARGET)) {
            return 0;
        }
        if (!FindTexFormat(m_texFormatShadow, TargetFormat, tfRgbaFormatsRts, tfRgbaBitsRts, 7, 16, D3DUSAGE_RENDERTARGET)) {
            return 0;
        }
        if (!FindTexFormat(m_texFormatDepth, TargetFormat, tfDepthFormats, tfDepthBits, 6, 32, D3DUSAGE_DEPTHSTENCIL) &&
            !FindTexFormat(m_texFormatDepth, TargetFormat, tfDepthFormats, tfDepthBits, 6, 16, D3DUSAGE_DEPTHSTENCIL)) {
            m_texFormatDepth = D3DFMT_UNKNOWN;
        }
        return 1;
    };

    bool CDevice::IsMultiSamplingSupported(int32_t numSamples) const {
        return this->m_pD3D->CheckDeviceMultiSampleType(
                   0, D3DDEVTYPE_HAL, this->m_d3dpp.BackBufferFormat, 0, (D3DMULTISAMPLE_TYPE)numSamples, 0
               ) >= 0 &&
               this->m_pD3D->CheckDeviceMultiSampleType(
                   0, D3DDEVTYPE_HAL, this->m_d3dpp.AutoDepthStencilFormat, 0, (D3DMULTISAMPLE_TYPE)numSamples, 0
               ) >= 0;
    };

    bool CDevice::IsMultiSamplingSupported(D3DFORMAT format, D3DMULTISAMPLE_TYPE multiSampleType, uint32_t* qualityLevels) const {
        return this->m_pD3D->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, format, 0, multiSampleType, (DWORD*)qualityLevels) >= 0;
    };

    int32_t CDevice::FindMultisampleType(
        D3DFORMAT targetFormat, D3DFORMAT depthStencilFormat, int32_t multiSamplesNum, D3DPRESENT_PARAMETERS& d3dpp
    ) {
        uint32_t qualityLevels = 0;

        HRESULT hr = m_pD3D->CheckDeviceMultiSampleType(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, targetFormat, FALSE, (D3DMULTISAMPLE_TYPE)multiSamplesNum, (DWORD*)&qualityLevels
        );

        if (FAILED(hr))
            return 0;

        hr = m_pD3D->CheckDeviceMultiSampleType(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, depthStencilFormat, FALSE, (D3DMULTISAMPLE_TYPE)multiSamplesNum, (DWORD*)&qualityLevels
        );

        if (FAILED(hr))
            return 0;

        d3dpp.MultiSampleType    = (D3DMULTISAMPLE_TYPE)multiSamplesNum;
        d3dpp.MultiSampleQuality = qualityLevels - 1;

        return 1;
    };

    D3DFORMAT CDevice::GetTexFormat(bool useAlpha, bool compressed) {
        return m_texFormat[useAlpha ? 0 : 1][compressed ? 0 : 1];
    }

    D3DCAPS9 CDevice::GetCaps() {
        return this->m_d3dCaps;
    };

    D3DFORMAT CDevice::GetTexFormatRt() {
        return this->m_texFormatRt;
    };

    D3DFORMAT CDevice::GetTexFormatShadow() {
        return this->m_texFormatShadow;
    };

    D3DFORMAT CDevice::GetTexFormatDepth() {
        return this->m_texFormatDepth;
    };

    D3DFORMAT CDevice::GetSurfaceFormat() {
        return this->m_surfaceFormat;
    };

    D3DFORMAT CDevice::GetDepthStencilFormat() {
        return this->m_depthStencilFormat;
    };

    HRESULT CDevice::setRenderState(D3DRENDERSTATETYPE state, uint32_t val) {
        if (this->m_curRenderState[state] == val)
            return 0;
        this->m_stats.swRenderStates++;
        this->m_curRenderState[state] = val;
        return this->m_pd3dDevice->SetRenderState(state, val);
    };

    uint32_t CDevice::getRenderState(D3DRENDERSTATETYPE state) {
        return this->m_curRenderState[state];
    };

    HRESULT CDevice::setTextureStageState(uint32_t stage, D3DTEXTURESTAGESTATETYPE type, uint32_t value) {
        if (this->m_curTexStagesStates[stage][type] == value)
            return 0;
        this->m_stats.swTextureStageStates++;
        this->m_curTexStagesStates[stage][type] = value;
        return this->m_pd3dDevice->SetTextureStageState(stage, type, value);
    };

    HRESULT CDevice::setTextureSamplerState(uint32_t stage, D3DSAMPLERSTATETYPE type, uint32_t value) {
        if (this->m_curTexSamplerStates[stage][type] == value)
            return 0;
        this->m_stats.swTextureSamplerStates++;
        this->m_curTexSamplerStates[stage][type] = value;
        return this->m_pd3dDevice->SetSamplerState(stage, type, value);
    };

    HRESULT CDevice::setStreamSourceFreq(uint32_t stage, uint32_t freq) {
        if (this->m_curStreamFreq[stage] == freq)
            return 0;
        this->m_curStreamFreq[stage] = freq;
        return this->m_pd3dDevice->SetStreamSourceFreq(stage, freq);
    };

    HRESULT CDevice::setFVF(unsigned int fvf) {
        if (this->m_curFVF == fvf)
            return 0;
        this->m_curFVF = fvf;
        return this->m_pd3dDevice->SetFVF(fvf);
    };

    HRESULT CDevice::setVertexDeclaration(IDirect3DVertexDeclaration9* vd) {
        if (vd == this->m_curVertexDecl)
            return 0;
        this->m_curVertexDecl = vd;
        return this->m_pd3dDevice->SetVertexDeclaration(vd);
    };

    HRESULT CDevice::setStreamSource(int32_t stream, IDirect3DVertexBuffer9* vb, uint32_t stride) {
        // if (this->m_curVb[stream] == vb && this->m_curStride[stream] == stride)
        //     return 0;
        this->m_curVb[stream]     = vb;
        this->m_curStride[stream] = stride;
        return this->m_pd3dDevice->SetStreamSource(stream, vb, 0, stride);
    };

    void CDevice::setIndices(IDirect3DIndexBuffer9* ib, unsigned int baseIdx) {
        this->m_latchedCheck     = 1;
        this->m_latchedIb        = ib;
        this->m_latchedIbBaseIdx = baseIdx;
    };

    HRESULT CDevice::setVertexShader(IDirect3DVertexShader9* shader) {
        // 1. Проверяем de-facto жив ли шейдер
        if (shader) {
            IDirect3DDevice9* devCheck = nullptr;
            HRESULT hr                 = shader->GetDevice(&devCheck);
            if (FAILED(hr) || !devCheck) {
                // Уже тут можно логнуть, что шейдер битый
                LOG_ERROR("VS GetDevice failed or dev == nullptr, hr=0x%08X", hr);
            } else {
                // Сравним, тот ли это девайс
                if (devCheck != this->m_pd3dDevice) {
                    LOG_ERROR("VS belongs to different device %p != %p", devCheck, this->m_pd3dDevice);
                }
                devCheck->Release();
            }
        }

        if (shader == this->m_curVertexShader)
            return 0;

        ++this->m_stats.swVertexShaders;
        this->m_curVertexShader = shader;
        return this->m_pd3dDevice->SetVertexShader(shader);
    }

    HRESULT CDevice::setPixelShader(IDirect3DPixelShader9* shader) {
        if (shader == this->m_curPixelShader)
            return 0;
        ++this->m_stats.swPixelShaders;
        this->m_curPixelShader = shader;
        return this->m_pd3dDevice->SetPixelShader(shader);
    };

    bool CDevice::IsIbValid(const IbHandle& ib) const {
        return ib.IsValid() && ib.m_handle < m_ibs.size() && m_ibs[ib.m_handle].m_ib != nullptr;
    };

    bool CDevice::IsMeshValid(const MeshHandle& mesh) const {
        return mesh.IsValid() && mesh.m_handle < m_meshes.size() && m_meshes[mesh.m_handle].m_mesh != nullptr;
    };

    bool CDevice::IsVbValid(const VbHandle& vb) const {
        return vb.IsValid() && vb.m_handle < m_vbs.size() && m_vbs[vb.m_handle].m_vb != nullptr;
    };

    bool CDevice::IsTexValid(const TexHandle& tex) const {
        Sampler* texture = this->mActiveTextures.GetItem(tex.m_handle);
        return texture && texture->m_maps.size();
    };

    CDevice::CDevice() {
        m_currentDXCursorInfo.m_texId.SetInvalid();
        InitMatrices();
        m_stateManager.SetDevice(this);
        G_DEVICE = this;
    }

    CDevice::~CDevice() {
        if (m_pSysFont) {
            m_pSysFont->Release();
            m_pSysFont = nullptr;
        }

        if (m_texFrameBufer.IsValid()) {
            ReleaseTexture(m_texFrameBufer);
            m_texFrameBufer.SetInvalid();
        }

        for (auto& [key, texHandle] : m_texBufferedRT) {
            ReleaseTexture(texHandle);
        }
        m_texBufferedRT.clear();

        if (m_currentDXCursorInfo.m_texId.IsValid()) {
            ReleaseTexture(m_currentDXCursorInfo.m_texId);
            m_currentDXCursorInfo.m_texId.SetInvalid();
        }

        for (auto& pair : this->mActiveTextures) {
            pair.second->m_maps.clear();
            pair.second->DecRef();
        }
        this->mActiveTextures.Reset();

        for (auto& pair : this->mActiveImages) {
            if (pair.second->mHandleBase) {
                pair.second->mHandleBase->Release();
                pair.second->mHandleBase = nullptr;
            }
            delete pair.second;
        }
        this->mActiveImages.Reset();

        DoneVertexDeclarations();
        doneFullScreenQuadStuff();

        for (auto& vbList : m_VbPoolBuffers) {
            for (auto& vbHandle : vbList) {
                ReleaseVb(vbHandle);
            }
        }
        m_VbPoolBuffers.clear();

        for (auto& ibHandle : m_IbPoolBuffers) {
            ReleaseIb(ibHandle);
        }
        m_IbPoolBuffers.clear();

        for (auto& ib : m_ibs) {
            if (ib.m_ib) {
                ib.m_ib->Release();
                ib.m_ib = nullptr;
            }
        }
        m_ibs.clear();

        for (auto& vb : m_vbs) {
            if (vb.m_vb) {
                vb.m_vb->Release();
                vb.m_vb = nullptr;
            }
        }
        m_vbs.clear();

        for (auto& mesh : m_meshes) {
            if (mesh.m_mesh) {
                mesh.m_mesh->Release();
                mesh.m_mesh = nullptr;
            }
        }
        m_meshes.clear();

        for (auto& [key, surface] : m_rtsZSurfaces) {
            if (surface) {
                surface->Release();
                surface = nullptr;
            }
        }
        m_rtsZSurfaces.clear();

        if (m_globalFxPool) {
            m_globalFxPool->Release();
            m_globalFxPool = nullptr;
        }

        if (m_NormalizingCubemap) {
            m_NormalizingCubemap->Release();
            m_NormalizingCubemap = nullptr;
        }

        if (m_SpecularPowerLookup) {
            m_SpecularPowerLookup->Release();
            m_SpecularPowerLookup = nullptr;
        }

        if (m_pd3dDevice) {
            m_pd3dDevice->Release();
            m_pd3dDevice = nullptr;
        }

        if (m_pD3D) {
            m_pD3D->Release();
            m_pD3D = nullptr;
        }

        ReleaseFsRt();

        m_queries.clear();

        m_d3dxMacros.clear();
        m_shadersMacros.clear();

        for (auto& [key, shader] : m_AsmShaders) {
            delete shader;
        }
        m_AsmShaders.clear();

        for (auto& [key, shader] : m_HlslShaders) {
            delete shader;
        }
        m_HlslShaders.clear();

        for (auto& [key, effect] : m_effects) {
            delete effect;
        }
        m_effects.clear();

        m_combinedVD.clear();
        m_resetCallbacks.clear();
        m_IbPoolFields.clear();
        m_VbPoolFields.clear();
        m_VbPoolTypes.clear();
    };

    IDirect3DDevice9* CDevice::GetDevice() {
        return m_pd3dDevice;
    };

    IDirect3D9* CDevice::GetInterface() {
        return m_pD3D;
    };

    int32_t CDevice::GetLastResult() const {
        return m_lastResult;
    };

    IDirect3DCubeTexture9* CDevice::GetNormalCubemap() {
        return this->m_NormalizingCubemap;
    };

    bool CDevice::Create(void (*)(const hta::CStr&), hta::m3d::Kernel*) {
        m_pD3D = Direct3DCreate9(D3D_SDK_VERSION);
        if (!m_pD3D) {
            LOG_ERROR("Unable to initialize D3D");
            return false;
        }

        _SetLastResult(m_pD3D->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &m_d3dCaps));
        if (FAILED(m_lastResult)) {
            LOG_ERROR("Unable to get device caps: 0x%08X", m_lastResult);
            return false;
        }

        if (m_d3dCaps.PixelShaderVersion < D3DPS_VERSION(1, 1)) {
            LOG_ERROR("Could not initialize 3d: no PS1.1 support found");
            return false;
        }

        DWORD textureCaps          = m_d3dCaps.TextureCaps;
        bool requiresPow2          = (textureCaps & D3DPTEXTURECAPS_POW2) != 0;
        bool hasConditionalNonPow2 = (textureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL) != 0;

        if (requiresPow2 && !hasConditionalNonPow2) {
            LOG_ERROR(
                "Device does not support non-power-of-2 textures (POW2=%d, NONPOW2CONDITIONAL=%d)", requiresPow2, hasConditionalNonPow2
            );
            return false;
        }

        _SetLastResult(D3DXCreateEffectPool(&m_globalFxPool));
        if (FAILED(m_lastResult)) {
            m_globalFxPool = nullptr;
            LOG_ERROR("Unable to create FX pool: 0x%08X", m_lastResult);
        }

        return true;
    };

    bool CDevice::CreateDevice() {
        _SetLastResult(m_pD3D->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &m_desktopMode));
        if (FAILED(m_lastResult)) {
            LOG_ERROR("Could not get current desktop format!");
            return false;
        }

        hta::m3d::EngineConfig& config = hta::m3d::Kernel::Instance()->GetEngineCfg();

        if (!FindSurfaceFormat(&m_surfaceFormat, config.m_r_bpp.m_i)) {
            LOG_ERROR("FindSurfaceFormat failed for %d", config.m_r_bpp.m_i);
            return false;
        }

        if (!FindTextureFormat(m_surfaceFormat, config.m_r_compressedTextures.m_i)) {
            LOG_ERROR("FindTextureFormat failed for %d", config.m_r_compressedTextures.m_i);
            return false;
        }

        if (!FindDepthFormat(m_surfaceFormat, &m_depthStencilFormat, config.m_r_depthBpp.m_i)) {
            LOG_ERROR("FindDepthFormat failed for %d", config.m_r_depthBpp.m_i);
            return false;
        }

        if (!FindDepthFormat(m_texFormatRt, &m_depthStencilFormatRt, 32)) {
            m_depthStencilFormatRt = D3DFMT_D16;
        }

        m_pd3dDevice = nullptr;
        ZeroMemory(&m_d3dpp, sizeof(m_d3dpp));

        m_d3dpp.BackBufferCount        = 2;
        m_d3dpp.PresentationInterval   = D3DPRESENT_INTERVAL_IMMEDIATE;
        m_d3dpp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
        m_d3dpp.EnableAutoDepthStencil = TRUE;

        bool isFullScreen =
            (config.m_r_fullScreen.m_type == hta::m3d::CVar::CVAR_BOOL) ? config.m_r_fullScreen.m_b : (config.m_r_fullScreen.m_i > 0);

        m_d3dpp.Windowed = !isFullScreen;

        // Create D3D device and switch display mode
        if (!SwitchDisplayModes(config.m_mainWnd, config.m_r_width.m_i, config.m_r_height.m_i, isFullScreen)) {
            return false;
        }

        loadShadersMacros();
        InitVertexDeclarations();
        CreateFsRt();
        doneFullScreenQuadStuff();
        initFullScreenQuad();

        if (m_pSysFont) {
            m_pSysFont->Release();
            m_pSysFont = nullptr;
        }

        if (FAILED(D3DXCreateFontA(
                m_pd3dDevice,
                15,
                0,
                FW_BOLD,
                1,
                FALSE,
                DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,
                DEFAULT_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE,
                "System",
                &m_pSysFont
            ))) {
            m_pSysFont = nullptr;
        }

        m_stencilLevel[0]     = -1;
        m_stencilLevel[1]     = -1;
        m_activeStencilTarget = 0;

        return true;
    };

    bool CDevice::SwitchDisplayModes(HWND wnd, int32_t w, int32_t h, bool fullscreen) {
        for (auto& [key, surface] : m_rtsZSurfaces) {
            if (surface) {
                surface->Release();
                surface = nullptr;
            }
        }

        m_rtsZSurfaces.clear();

        return SwitchDisplayModes0(wnd, w, h, fullscreen);
    };

    static bool wasReset = false;

    bool CDevice::Reset() {
        if (m_pSysFont) {
            m_pSysFont->OnLostDevice();
        }

        if (!wasReset) {
            for (auto* callback : m_resetCallbacks) {
                callback->OnBeforeDeviceReset();
            }

            for (auto& [key, surface] : m_rtsZSurfaces) {
                if (surface) {
                    surface->Release();
                    surface = nullptr;
                }
            }
            m_rtsZSurfaces.clear();

            rstShadersPrepareFor();
            rstTexPrepareFor();
            rstIbPrepareFor();
            rstVbPrepareFor();
            rstQueryPrepareFor();
            ShowDXCursor(true);

            wasReset = true;
        }

        hta::m3d::EngineConfig& config = hta::m3d::Kernel::Instance()->GetEngineCfg();

        if (!FindMultisampleType(
                m_d3dpp.BackBufferFormat,
                m_d3dpp.AutoDepthStencilFormat,
                static_cast<D3DMULTISAMPLE_TYPE>(config.m_r_multiSamplesNum.m_i),
                m_d3dpp
            )) {
            m_d3dpp.MultiSampleType    = D3DMULTISAMPLE_NONE;
            m_d3dpp.MultiSampleQuality = 0;
        }

        _SetLastResult(m_pd3dDevice->Reset(&m_d3dpp));

        if (FAILED(m_lastResult)) {
            LOG_ERROR("d3d device reset failed: 0x%08X", m_lastResult);
            return false;
        }

        LOG_INFO("Device reset ok");

        if (wasReset) {
            if (m_pSysFont) {
                m_pSysFont->OnResetDevice();
            }

            rstShadersRestoreAfter();
            rstVbRestoreAfter();
            rstIbRestoreAfter();
            rstTexRestoreAfter();
            rstQueryRestoreAfter();

            ReleaseTexture(m_texFrameBufer);

            for (auto* callback : m_resetCallbacks) {
                callback->OnAfterDeviceReset();
            }

            internalReset();
            wasReset = false;
            InitMatrices();
        }

        bool useDxCursor =
            (config.m_r_dxcursor.m_type == hta::m3d::CVar::CVAR_BOOL) ? config.m_r_dxcursor.m_b : config.m_r_dxcursor.m_i > 0;

        if (useDxCursor) {
            SetupDXCursorForce(m_currentDXCursorInfo.m_texId, m_currentDXCursorInfo.m_xHotSpot, m_currentDXCursorInfo.m_yHotSpot, false);
        }

        m_stencilLevel[0] = -1;
        m_stencilLevel[1] = -1;

        if (m_d3dCaps.VertexShaderVersion < D3DVS_VERSION(3, 0)) {
            if (!m_pD3D->CheckDeviceFormat(
                    D3DADAPTER_DEFAULT,
                    D3DDEVTYPE_HAL,
                    D3DFMT_X8R8G8B8,
                    0,
                    D3DRTYPE_SURFACE,
                    static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'))
                )) {
                m_pd3dDevice->SetRenderState(D3DRS_POINTSIZE, MAKEFOURCC('I', 'N', 'T', 'Z'));
            }
        }

        return true;
    };

    int32_t CDevice::SwitchDisplayModes0(HWND wnd, int32_t w, int32_t h, int32_t fullscreen) {
        IDirect3DSurface9* pBackBuffer   = nullptr;
        IDirect3DSurface9* pDepthStencil = nullptr;
        Viewport port                    = {0, 0, w, h, 0.0f, 1.0f};

        m_d3dpp.hDeviceWindow    = wnd;
        m_d3dpp.BackBufferWidth  = w;
        m_d3dpp.BackBufferHeight = h;

        if (fullscreen) {
            m_d3dpp.Windowed               = FALSE;
            m_d3dpp.BackBufferFormat       = m_surfaceFormat;
            m_d3dpp.AutoDepthStencilFormat = m_depthStencilFormat;
        } else {
            m_d3dpp.Windowed         = TRUE;
            m_d3dpp.BackBufferFormat = m_desktopMode.Format;

            int depthBpp =
                (m_desktopMode.Format == D3DFMT_D32 || m_desktopMode.Format == D3DFMT_D24S8 || m_desktopMode.Format == D3DFMT_D24X4S4) ? 32
                                                                                                                                       : 16;

            if (!FindDepthFormat(m_desktopMode.Format, &m_d3dpp.AutoDepthStencilFormat, depthBpp)) {
                LOG_ERROR("CDevice::FindDepthFormat(%d, FORMAT, %d) failed", m_desktopMode.Format, depthBpp);
                return false;
            }
        }

        if (m_pd3dDevice) {
            if (!Reset()) {
                LOG_ERROR("Reset or CreateDevice error: %s", GetLastErrorStr());
                return false;
            }

            IDirect3DSurface9* depthCheck = nullptr;
            m_pd3dDevice->GetDepthStencilSurface(&depthCheck);
            LOG_INFO("After Reset: Depth buffer = %p", depthCheck);
            if (depthCheck)
                depthCheck->Release();

            _SetLastResult(m_pd3dDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer));
            pBackBuffer->GetDesc(&m_d3dsdBackBuffer);
            pBackBuffer->Release();

            _SetLastResult(m_pd3dDevice->GetDepthStencilSurface(&pDepthStencil));
            D3DSURFACE_DESC depthDesc;
            pDepthStencil->GetDesc(&depthDesc);
            pDepthStencil->Release();

            SetViewport(port);

            LOG_INFO("Creating or resetting the device");
            LOG_INFO("Back buffer format: %d", m_d3dsdBackBuffer.Format);
            LOG_INFO("Depth/stencil buffer format: %d", depthDesc.Format);
            LOG_INFO("Stenciling: %s", m_haveStencil ? "on" : "off");
            LOG_INFO("Rgba textures format: %d", m_texFormat[0][0]);
            LOG_INFO("Rgba no c. textures format: %d", m_texFormat[0][1]);
            LOG_INFO("Rgb textures format: %d", m_texFormat[1][0]);
            LOG_INFO("Rgb no c. textures format: %d", m_texFormat[1][1]);
            LOG_INFO("Rt textures format: %d", m_texFormatRt);
            LOG_INFO("Shadow textures format: %d", m_texFormatShadow);
            LOG_INFO("Depth textures format: %d", m_texFormatDepth);
            LOG_INFO("Rt depth format: %d", m_depthStencilFormatRt);

            return true;
        }

        hta::m3d::EngineConfig& config = hta::m3d::Kernel::Instance()->GetEngineCfg();

        if (!FindMultisampleType(
                m_d3dpp.BackBufferFormat,
                m_d3dpp.AutoDepthStencilFormat,
                static_cast<D3DMULTISAMPLE_TYPE>(config.m_r_multiSamplesNum.m_i),
                m_d3dpp
            )) {
            m_d3dpp.MultiSampleType    = D3DMULTISAMPLE_NONE;
            m_d3dpp.MultiSampleQuality = 0;
        }

        _SetLastResult(m_pD3D->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &m_d3dCaps));
        if (FAILED(m_lastResult)) {
            LOG_ERROR("Could not get device caps for the default device, hr = 0x%x", m_lastResult);
            return false;
        }

        DWORD devCaps      = m_d3dCaps.DevCaps;
        bool hasHwTnL      = (devCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) != 0;
        bool hasPureDevice = (devCaps & D3DDEVCAPS_PUREDEVICE) != 0 && (devCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) != 0;

        DWORD requestedBehavior = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
        DWORD actualBehavior    = requestedBehavior;

        if (!hasHwTnL) {
            actualBehavior = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
            LOG_WARNING("Hardware T&L not available, falling back to software VP!");
        }

        LOG_INFO("Given device behavior: %d", requestedBehavior);
        LOG_INFO("Confirmed device behavior: %d", actualBehavior);

        UINT adapter          = D3DADAPTER_DEFAULT;
        D3DDEVTYPE deviceType = D3DDEVTYPE_HAL;

        for (UINT i = 0; i < m_pD3D->GetAdapterCount(); i++) {
            D3DADAPTER_IDENTIFIER9 adapterId;
            m_pD3D->GetAdapterIdentifier(i, 0, &adapterId);

            if (strcmp(adapterId.Description, "NVIDIA NVPerfHUD") == 0) {
                adapter    = i;
                deviceType = D3DDEVTYPE_REF;
                LOG_INFO("NVIDIA NVPerfHUD detected!!!");
                break;
            }
        }

        _SetLastResult(m_pD3D->CreateDevice(adapter, deviceType, config.m_mainWnd, actualBehavior, &m_d3dpp, &m_pd3dDevice));

        if (FAILED(m_lastResult)) {
            LOG_ERROR("error creating device: %s", GetLastErrorStr());
            return false;
        }

        this->mVSUniforms.Attach(this->m_pd3dDevice);
        this->mFSUniforms.Attach(this->m_pd3dDevice);

        IDirect3DSurface9* depthCheck2 = nullptr;
        m_pd3dDevice->GetDepthStencilSurface(&depthCheck2);
        LOG_INFO("After CreateDevice: Depth buffer = %p", depthCheck2);
        LOG_INFO("  EnableAutoDepthStencil = %d", m_d3dpp.EnableAutoDepthStencil);
        LOG_INFO("  AutoDepthStencilFormat = %d", m_d3dpp.AutoDepthStencilFormat);
        if (depthCheck2)
            depthCheck2->Release();

        D3DADAPTER_IDENTIFIER9 id;
        m_pD3D->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);

        LOG_INFO("Description: %s", id.Description);
        LOG_INFO("Driver:      %s", id.Driver);
        LOG_INFO("VendorId:    0x%x", id.VendorId);
        LOG_INFO("DeviceId:    0x%x", id.DeviceId);
        LOG_INFO("SubSysId:    0x%x", id.SubSysId);
        LOG_INFO("Revision:    %d", id.Revision);
        LOG_INFO("Driver version info:");
        LOG_INFO("  Product:    %d", HIWORD(id.DriverVersion.HighPart));
        LOG_INFO("  Version:    %d", LOWORD(id.DriverVersion.HighPart));
        LOG_INFO("  SubVersion: %d", HIWORD(id.DriverVersion.LowPart));
        LOG_INFO("  Build:      %d", LOWORD(id.DriverVersion.LowPart));
        LOG_INFO("WHQLLevel:    %d", id.WHQLLevel);

        _SetLastResult(m_pd3dDevice->GetDeviceCaps(&m_d3dCaps));

        SetupAllFeaturesSupport("data\\DeviceCompatible.xml");
        logCaps();

        m_isNV30 =
            (id.VendorId == 0x10DE) && ((id.DeviceId >= 0x301 && id.DeviceId < 0x330) || (id.DeviceId > 0x334 && id.DeviceId < 0x400));

        internalReset();

        _SetLastResult(m_pd3dDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer));
        pBackBuffer->GetDesc(&m_d3dsdBackBuffer);
        pBackBuffer->Release();

        _SetLastResult(m_pd3dDevice->GetDepthStencilSurface(&pDepthStencil));
        D3DSURFACE_DESC depthDesc;
        pDepthStencil->GetDesc(&depthDesc);
        pDepthStencil->Release();

        SetViewport(port);

        LOG_INFO("Creating or resetting the device");
        LOG_INFO("Back buffer format: %d", m_d3dsdBackBuffer.Format);
        LOG_INFO("Depth/stencil buffer format: %d", depthDesc.Format);
        LOG_INFO("Stenciling: %s", m_haveStencil ? "on" : "off");
        LOG_INFO("Rgba textures format: %d", m_texFormat[0][0]);
        LOG_INFO("Rgba no c. textures format: %d", m_texFormat[0][1]);
        LOG_INFO("Rgb textures format: %d", m_texFormat[1][0]);
        LOG_INFO("Rgb no c. textures format: %d", m_texFormat[1][1]);
        LOG_INFO("Rt textures format: %d", m_texFormatRt);
        LOG_INFO("Shadow textures format: %d", m_texFormatShadow);
        LOG_INFO("Depth textures format: %d", m_texFormatDepth);
        LOG_INFO("Rt depth format: %d", m_depthStencilFormatRt);

        return true;
    };

    void CDevice::PushAlphaTest(int32_t value) {
        bool changed = (m_stackAlphaTest[m_stackTopAlphaTest] != value);
        DuplicateAlphaTest();
        SetAlphaTest(value, changed);
    };

    void CDevice::DuplicateAlphaTest() {
        _PROTECT_OVERFLOW(m_stackTopAlphaTest);
        ++m_stackTopAlphaTest;
        m_stackAlphaTest[m_stackTopAlphaTest] = m_stackAlphaTest[m_stackTopAlphaTest - 1];
    };

    void CDevice::PopAlphaTest() {
        _PROTECT_UNDERFLOW(m_stackTopAlphaTest);
        --m_stackTopAlphaTest;
        SetAlphaTest(m_stackAlphaTest[m_stackTopAlphaTest], false);
    };

    void CDevice::SetAlphaTest(int value, bool force) {
        m_stackAlphaTest[m_stackTopAlphaTest] = value;

        if (value <= 0 || (m_d3dCaps.AlphaCmpCaps & D3DPCMPCAPS_GREATEREQUAL) == 0) {
            if (m_curRenderState[D3DRS_ALPHATESTENABLE] != 0) {
                ++m_stats.swRenderStates;
                m_curRenderState[D3DRS_ALPHATESTENABLE] = 0;
                m_pd3dDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            }
            return;
        }

        if (m_curRenderState[D3DRS_ALPHATESTENABLE] != 1) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_ALPHATESTENABLE] = 1;
            m_pd3dDevice->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        }

        if (m_curRenderState[D3DRS_ALPHAREF] != value) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_ALPHAREF] = value;
            m_pd3dDevice->SetRenderState(D3DRS_ALPHAREF, value);
        }

        if (m_curRenderState[D3DRS_ALPHAFUNC] != D3DCMP_GREATEREQUAL) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_ALPHAFUNC] = D3DCMP_GREATEREQUAL;
            m_pd3dDevice->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        }
    };

    void CDevice::SetAlphaTest(int32_t value) {
        SetAlphaTest(value, true);
    };

    void CDevice::PushBlend(BlendMode mode) {
        bool changed = (m_stackBlend[m_stackTopBlend] != mode);
        DuplicateBlend();
        SetBlend(mode, changed);
    };

    void CDevice::DuplicateBlend() {
        _PROTECT_OVERFLOW(m_stackTopBlend);
        ++m_stackTopBlend;
        m_stackBlend[m_stackTopBlend] = m_stackBlend[m_stackTopBlend - 1];
    }

    void CDevice::PopBlend() {
        _PROTECT_UNDERFLOW(m_stackTopBlend);
        --m_stackTopBlend;
        SetBlend(m_stackBlend[m_stackTopBlend], false);
    };

    static D3DBLEND srcDst[124][2]{
        {
            D3DBLEND_ZERO,
            D3DBLEND_ZERO,
        },
        {
            D3DBLEND_ONE,
            D3DBLEND_ONE,
        },
        {
            D3DBLEND_SRCALPHA,
            D3DBLEND_INVSRCALPHA,
        },
        {
            D3DBLEND_ZERO,
            D3DBLEND_ZERO,
        },
        {
            D3DBLEND_ZERO,
            D3DBLEND_ONE,
        },
        {
            D3DBLEND_ZERO,
            D3DBLEND_SRCCOLOR,
        },
        {
            D3DBLEND_ZERO,
            D3DBLEND_INVSRCCOLOR,
        },
        {
            D3DBLEND_ZERO,
            D3DBLEND_SRCALPHA,
        },
        {
            D3DBLEND_ZERO,
            D3DBLEND_INVSRCALPHA,
        },
        {
            D3DBLEND_ZERO,
            D3DBLEND_DESTALPHA,
        },
        {
            D3DBLEND_ZERO,
            D3DBLEND_INVDESTALPHA,
        },
        {
            D3DBLEND_ZERO,
            D3DBLEND_DESTCOLOR,
        },
        {
            D3DBLEND_ZERO,
            D3DBLEND_INVDESTCOLOR,
        },
        {
            D3DBLEND_ZERO,
            D3DBLEND_SRCALPHASAT,
        },
        {
            D3DBLEND_ONE,
            D3DBLEND_ZERO,
        },
        {
            D3DBLEND_ONE,
            D3DBLEND_ONE,
        },
        {
            D3DBLEND_ONE,
            D3DBLEND_SRCCOLOR,
        },
        {
            D3DBLEND_ONE,
            D3DBLEND_INVSRCCOLOR,
        },
        {
            D3DBLEND_ONE,
            D3DBLEND_SRCALPHA,
        },
        {
            D3DBLEND_ONE,
            D3DBLEND_INVSRCALPHA,
        },
        {
            D3DBLEND_ONE,
            D3DBLEND_DESTALPHA,
        },
        {
            D3DBLEND_ONE,
            D3DBLEND_INVDESTALPHA,
        },
        {
            D3DBLEND_ONE,
            D3DBLEND_DESTCOLOR,
        },
        {
            D3DBLEND_ONE,
            D3DBLEND_INVDESTCOLOR,
        },
        {
            D3DBLEND_ONE,
            D3DBLEND_SRCALPHASAT,
        },
        {
            D3DBLEND_SRCCOLOR,
            D3DBLEND_ZERO,
        },
        {
            D3DBLEND_SRCCOLOR,
            D3DBLEND_ONE,
        },
        {
            D3DBLEND_SRCCOLOR,
            D3DBLEND_SRCCOLOR,
        },
        {
            D3DBLEND_SRCCOLOR,
            D3DBLEND_INVSRCCOLOR,
        },
        {
            D3DBLEND_SRCCOLOR,
            D3DBLEND_SRCALPHA,
        },
        {
            D3DBLEND_SRCCOLOR,
            D3DBLEND_INVSRCALPHA,
        },
        {
            D3DBLEND_SRCCOLOR,
            D3DBLEND_DESTALPHA,
        },
        {
            D3DBLEND_SRCCOLOR,
            D3DBLEND_INVDESTALPHA,
        },
        {
            D3DBLEND_SRCCOLOR,
            D3DBLEND_DESTCOLOR,
        },
        {
            D3DBLEND_SRCCOLOR,
            D3DBLEND_INVDESTCOLOR,
        },
        {
            D3DBLEND_SRCCOLOR,
            D3DBLEND_SRCALPHASAT,
        },
        {
            D3DBLEND_INVSRCCOLOR,
            D3DBLEND_ZERO,
        },
        {
            D3DBLEND_INVSRCCOLOR,
            D3DBLEND_ONE,
        },
        {
            D3DBLEND_INVSRCCOLOR,
            D3DBLEND_SRCCOLOR,
        },
        {
            D3DBLEND_INVSRCCOLOR,
            D3DBLEND_INVSRCCOLOR,
        },
        {
            D3DBLEND_INVSRCCOLOR,
            D3DBLEND_SRCALPHA,
        },
        {
            D3DBLEND_INVSRCCOLOR,
            D3DBLEND_INVSRCALPHA,
        },
        {
            D3DBLEND_INVSRCCOLOR,
            D3DBLEND_DESTALPHA,
        },
        {
            D3DBLEND_INVSRCCOLOR,
            D3DBLEND_INVDESTALPHA,
        },
        {
            D3DBLEND_INVSRCCOLOR,
            D3DBLEND_DESTCOLOR,
        },
        {
            D3DBLEND_INVSRCCOLOR,
            D3DBLEND_INVDESTCOLOR,
        },
        {
            D3DBLEND_INVSRCCOLOR,
            D3DBLEND_SRCALPHASAT,
        },
        {
            D3DBLEND_SRCALPHA,
            D3DBLEND_ZERO,
        },
        {
            D3DBLEND_SRCALPHA,
            D3DBLEND_ONE,
        },
        {
            D3DBLEND_SRCALPHA,
            D3DBLEND_SRCCOLOR,
        },
        {
            D3DBLEND_SRCALPHA,
            D3DBLEND_INVSRCCOLOR,
        },
        {
            D3DBLEND_SRCALPHA,
            D3DBLEND_SRCALPHA,
        },
        {
            D3DBLEND_SRCALPHA,
            D3DBLEND_INVSRCALPHA,
        },
        {
            D3DBLEND_SRCALPHA,
            D3DBLEND_DESTALPHA,
        },
        {
            D3DBLEND_SRCALPHA,
            D3DBLEND_INVDESTALPHA,
        },
        {
            D3DBLEND_SRCALPHA,
            D3DBLEND_DESTCOLOR,
        },
        {
            D3DBLEND_SRCALPHA,
            D3DBLEND_INVDESTCOLOR,
        },
        {
            D3DBLEND_SRCALPHA,
            D3DBLEND_SRCALPHASAT,
        },
        {
            D3DBLEND_INVSRCALPHA,
            D3DBLEND_ZERO,
        },
        {
            D3DBLEND_INVSRCALPHA,
            D3DBLEND_ONE,
        },
        {
            D3DBLEND_INVSRCALPHA,
            D3DBLEND_SRCCOLOR,
        },
        {
            D3DBLEND_INVSRCALPHA,
            D3DBLEND_INVSRCCOLOR,
        },
        {
            D3DBLEND_INVSRCALPHA,
            D3DBLEND_SRCALPHA,
        },
        {
            D3DBLEND_INVSRCALPHA,
            D3DBLEND_INVSRCALPHA,
        },
        {
            D3DBLEND_INVSRCALPHA,
            D3DBLEND_DESTALPHA,
        },
        {
            D3DBLEND_INVSRCALPHA,
            D3DBLEND_INVDESTALPHA,
        },
        {
            D3DBLEND_INVSRCALPHA,
            D3DBLEND_DESTCOLOR,
        },
        {
            D3DBLEND_INVSRCALPHA,
            D3DBLEND_INVDESTCOLOR,
        },
        {
            D3DBLEND_INVSRCALPHA,
            D3DBLEND_SRCALPHASAT,
        },
        {
            D3DBLEND_DESTALPHA,
            D3DBLEND_ZERO,
        },
        {
            D3DBLEND_DESTALPHA,
            D3DBLEND_ONE,
        },
        {
            D3DBLEND_DESTALPHA,
            D3DBLEND_SRCCOLOR,
        },
        {
            D3DBLEND_DESTALPHA,
            D3DBLEND_INVSRCCOLOR,
        },
        {
            D3DBLEND_DESTALPHA,
            D3DBLEND_SRCALPHA,
        },
        {
            D3DBLEND_DESTALPHA,
            D3DBLEND_INVSRCALPHA,
        },
        {
            D3DBLEND_DESTALPHA,
            D3DBLEND_DESTALPHA,
        },
        {
            D3DBLEND_DESTALPHA,
            D3DBLEND_INVDESTALPHA,
        },
        {
            D3DBLEND_DESTALPHA,
            D3DBLEND_DESTCOLOR,
        },
        {
            D3DBLEND_DESTALPHA,
            D3DBLEND_INVDESTCOLOR,
        },
        {
            D3DBLEND_DESTALPHA,
            D3DBLEND_SRCALPHASAT,
        },
        {
            D3DBLEND_INVDESTALPHA,
            D3DBLEND_ZERO,
        },
        {
            D3DBLEND_INVDESTALPHA,
            D3DBLEND_ONE,
        },
        {
            D3DBLEND_INVDESTALPHA,
            D3DBLEND_SRCCOLOR,
        },
        {
            D3DBLEND_INVDESTALPHA,
            D3DBLEND_INVSRCCOLOR,
        },
        {
            D3DBLEND_INVDESTALPHA,
            D3DBLEND_SRCALPHA,
        },
        {
            D3DBLEND_INVDESTALPHA,
            D3DBLEND_INVSRCALPHA,
        },
        {
            D3DBLEND_INVDESTALPHA,
            D3DBLEND_DESTALPHA,
        },
        {
            D3DBLEND_INVDESTALPHA,
            D3DBLEND_INVDESTALPHA,
        },
        {
            D3DBLEND_INVDESTALPHA,
            D3DBLEND_DESTCOLOR,
        },
        {
            D3DBLEND_INVDESTALPHA,
            D3DBLEND_INVDESTCOLOR,
        },
        {
            D3DBLEND_INVDESTALPHA,
            D3DBLEND_SRCALPHASAT,
        },
        {
            D3DBLEND_DESTCOLOR,
            D3DBLEND_ZERO,
        },
        {
            D3DBLEND_DESTCOLOR,
            D3DBLEND_ONE,
        },
        {
            D3DBLEND_DESTCOLOR,
            D3DBLEND_SRCCOLOR,
        },
        {
            D3DBLEND_DESTCOLOR,
            D3DBLEND_INVSRCCOLOR,
        },
        {
            D3DBLEND_DESTCOLOR,
            D3DBLEND_SRCALPHA,
        },
        {
            D3DBLEND_DESTCOLOR,
            D3DBLEND_INVSRCALPHA,
        },
        {
            D3DBLEND_DESTCOLOR,
            D3DBLEND_DESTALPHA,
        },
        {
            D3DBLEND_DESTCOLOR,
            D3DBLEND_INVDESTALPHA,
        },
        {
            D3DBLEND_DESTCOLOR,
            D3DBLEND_DESTCOLOR,
        },
        {
            D3DBLEND_DESTCOLOR,
            D3DBLEND_INVDESTCOLOR,
        },
        {
            D3DBLEND_DESTCOLOR,
            D3DBLEND_SRCALPHASAT,
        },
        {
            D3DBLEND_INVDESTCOLOR,
            D3DBLEND_ZERO,
        },
        {
            D3DBLEND_INVDESTCOLOR,
            D3DBLEND_ONE,
        },
        {
            D3DBLEND_INVDESTCOLOR,
            D3DBLEND_SRCCOLOR,
        },
        {
            D3DBLEND_INVDESTCOLOR,
            D3DBLEND_INVSRCCOLOR,
        },
        {
            D3DBLEND_INVDESTCOLOR,
            D3DBLEND_SRCALPHA,
        },
        {
            D3DBLEND_INVDESTCOLOR,
            D3DBLEND_INVSRCALPHA,
        },
        {
            D3DBLEND_INVDESTCOLOR,
            D3DBLEND_DESTALPHA,
        },
        {
            D3DBLEND_INVDESTCOLOR,
            D3DBLEND_INVDESTALPHA,
        },
        {
            D3DBLEND_INVDESTCOLOR,
            D3DBLEND_DESTCOLOR,
        },
        {
            D3DBLEND_INVDESTCOLOR,
            D3DBLEND_INVDESTCOLOR,
        },
        {
            D3DBLEND_INVDESTCOLOR,
            D3DBLEND_SRCALPHASAT,
        },
        {
            D3DBLEND_SRCALPHASAT,
            D3DBLEND_ZERO,
        },
        {
            D3DBLEND_SRCALPHASAT,
            D3DBLEND_ONE,
        },
        {
            D3DBLEND_SRCALPHASAT,
            D3DBLEND_SRCCOLOR,
        },
        {
            D3DBLEND_SRCALPHASAT,
            D3DBLEND_INVSRCCOLOR,
        },
        {
            D3DBLEND_SRCALPHASAT,
            D3DBLEND_SRCALPHA,
        },
        {
            D3DBLEND_SRCALPHASAT,
            D3DBLEND_INVSRCALPHA,
        },
        {
            D3DBLEND_SRCALPHASAT,
            D3DBLEND_DESTALPHA,
        },
        {
            D3DBLEND_SRCALPHASAT,
            D3DBLEND_INVDESTALPHA,
        },
        {
            D3DBLEND_SRCALPHASAT,
            D3DBLEND_DESTCOLOR,
        },
        {
            D3DBLEND_SRCALPHASAT,
            D3DBLEND_INVDESTCOLOR,
        },
        {
            D3DBLEND_SRCALPHASAT,
            D3DBLEND_SRCALPHASAT,
        },
    };

    void CDevice::SetBlend(BlendMode mode, bool force) {
        m_stackBlend[m_stackTopBlend] = mode;

        if (mode != BM_NONE) {
            if (m_curRenderState[D3DRS_ALPHABLENDENABLE] != 1) {
                ++m_stats.swRenderStates;
                m_curRenderState[D3DRS_ALPHABLENDENABLE] = 1;
                m_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            }

            D3DBLEND srcBlend = srcDst[mode][0];
            if (m_curRenderState[D3DRS_SRCBLEND] != srcBlend) {
                ++m_stats.swRenderStates;
                m_curRenderState[D3DRS_SRCBLEND] = srcBlend;
                m_pd3dDevice->SetRenderState(D3DRS_SRCBLEND, srcBlend);
            }

            D3DBLEND dstBlend = srcDst[mode][1];
            if (m_curRenderState[D3DRS_DESTBLEND] != dstBlend) {
                ++m_stats.swRenderStates;
                m_curRenderState[D3DRS_DESTBLEND] = dstBlend;
                m_pd3dDevice->SetRenderState(D3DRS_DESTBLEND, dstBlend);
            }
        } else {
            if (m_curRenderState[D3DRS_ALPHABLENDENABLE] != 0) {
                ++m_stats.swRenderStates;
                m_curRenderState[D3DRS_ALPHABLENDENABLE] = 0;
                m_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            }
        }
    };

    void CDevice::PushZbState(ZbState state) {
        bool changed = (m_stackZbState[m_stackTopZbState] != state);
        DuplicateZbState();
        SetZbState(state, changed);
    };

    void CDevice::DuplicateZbState() {
        _PROTECT_OVERFLOW(m_stackTopZbState);
        ++m_stackTopZbState;
        m_stackZbState[m_stackTopZbState] = m_stackZbState[m_stackTopZbState - 1];
    };

    void CDevice::PopZbState() {
        _PROTECT_UNDERFLOW(m_stackTopZbState);
        --m_stackTopZbState;
        SetZbState(m_stackZbState[m_stackTopZbState], false);
    };

    void CDevice::SetZbState(ZbState state, bool force) {
        m_stackZbState[m_stackTopZbState] = state;

        switch (state) {
        case ZB_DISABLE:
            setRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
            break;

        case ZB_ENABLE:
            setRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
            setRenderState(D3DRS_ZWRITEENABLE, TRUE);
            setRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            break;

        case ZB_NOWRITE:
            setRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
            setRenderState(D3DRS_ZWRITEENABLE, FALSE);
            setRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            break;

        case ZB_WRITE_NOTEST:
            setRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
            setRenderState(D3DRS_ZWRITEENABLE, TRUE);
            setRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
            break;

        default:
            break;
        }
    };

    void CDevice::PushCull(Cull mode) {
        bool changed = (m_stackCull[m_stackTopCull] != mode);
        DuplicateCull();
        SetCull(mode, changed);
    }

    void CDevice::DuplicateCull() {
        _PROTECT_OVERFLOW(m_stackTopCull);
        ++m_stackTopCull;
        m_stackCull[m_stackTopCull] = m_stackCull[m_stackTopCull - 1];
    };

    void CDevice::PopCull() {
        _PROTECT_UNDERFLOW(m_stackTopCull);
        --m_stackTopCull;
        SetCull(m_stackCull[m_stackTopCull], false);
    };

    static D3DCULL m3dCullToD3dCull[3]{
        D3DCULL_NONE,
        D3DCULL_CW,
        D3DCULL_CCW,
    };

    void CDevice::SetCull(Cull mode, bool force) {
        m_stackCull[m_stackTopCull] = mode;

        D3DCULL cullMode = m3dCullToD3dCull[mode];
        if (m_curRenderState[D3DRS_CULLMODE] != cullMode) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_CULLMODE] = cullMode;
            m_pd3dDevice->SetRenderState(D3DRS_CULLMODE, cullMode);
        }
    }
    void CDevice::PushZFunc(CmpFunc func) {
        bool changed = (m_stackZFunc[m_stackTopZFunc] != func);
        DuplicateZFunc();
        SetZFunc(func, changed);
    };

    void CDevice::DuplicateZFunc() {
        _PROTECT_OVERFLOW(m_stackTopZFunc);
        ++m_stackTopZFunc;
        m_stackZFunc[m_stackTopZFunc] = m_stackZFunc[m_stackTopZFunc - 1];
    };

    void CDevice::PopZFunc() {
        _PROTECT_UNDERFLOW(m_stackTopZFunc);
        --m_stackTopZFunc;
        SetZFunc(m_stackZFunc[m_stackTopZFunc], false);
    };

    static D3DCMPFUNC m3dCmpToD3dCmp[8]{
        D3DCMP_NEVER,
        D3DCMP_LESS,
        D3DCMP_EQUAL,
        D3DCMP_LESSEQUAL,
        D3DCMP_GREATER,
        D3DCMP_NOTEQUAL,
        D3DCMP_GREATEREQUAL,
        D3DCMP_ALWAYS,
    };

    void CDevice::SetZFunc(CmpFunc func, bool force) {
        m_stackZFunc[m_stackTopZFunc] = func;

        D3DCMPFUNC cmpFunc = m3dCmpToD3dCmp[func];
        if (m_curRenderState[D3DRS_ZFUNC] != cmpFunc) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_ZFUNC] = cmpFunc;
            m_pd3dDevice->SetRenderState(D3DRS_ZFUNC, cmpFunc);
        }
    };

    void CDevice::PushLighting(bool enabled) {
        bool changed = (m_stackLighting[m_stackTopLighting] != enabled);
        DuplicateLighting();
        SetLighting(enabled, changed);
    }

    void CDevice::DuplicateLighting() {
        _PROTECT_OVERFLOW(m_stackTopLighting);
        ++m_stackTopLighting;
        m_stackLighting[m_stackTopLighting] = m_stackLighting[m_stackTopLighting - 1];
    };

    void CDevice::PopLighting() {
        _PROTECT_UNDERFLOW(m_stackTopLighting);
        --m_stackTopLighting;
        SetLighting(m_stackLighting[m_stackTopLighting], false);
    };

    void CDevice::SetLighting(bool enabled, bool force) {
        m_stackLighting[m_stackTopLighting] = enabled;

        if (m_curRenderState[D3DRS_LIGHTING] != enabled) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_LIGHTING] = enabled;
            m_pd3dDevice->SetRenderState(D3DRS_LIGHTING, enabled);
        }
    };

    void CDevice::PushAmbient(uint32_t color) {
        bool changed = (m_stackAmbient[m_stackTopAmbient] != color);
        DuplicateAmbient();
        SetAmbient(color, changed);
    };

    void CDevice::DuplicateAmbient() {
        _PROTECT_OVERFLOW(m_stackTopAmbient);
        ++m_stackTopAmbient;
        m_stackAmbient[m_stackTopAmbient] = m_stackAmbient[m_stackTopAmbient - 1];
    };

    void CDevice::PopAmbient() {
        _PROTECT_UNDERFLOW(m_stackTopAmbient);
        --m_stackTopAmbient;
        SetAmbient(m_stackAmbient[m_stackTopAmbient], false);
    };

    void CDevice::SetAmbient(uint32_t color, bool force) {
        m_stackAmbient[m_stackTopAmbient] = color;

        if (m_curRenderState[D3DRS_AMBIENT] != color) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_AMBIENT] = color;
            m_pd3dDevice->SetRenderState(D3DRS_AMBIENT, color);
        }
    };

    void CDevice::PushFog(bool enabled) {
        bool changed = (m_stackFog[m_stackTopFog] != enabled);
        DuplicateFog();
        SetFog(enabled, changed);
    };

    void CDevice::DuplicateFog() {
        _PROTECT_OVERFLOW(m_stackTopFog);
        ++m_stackTopFog;
        m_stackFog[m_stackTopFog] = m_stackFog[m_stackTopFog - 1];
    };

    void CDevice::PopFog() {
        _PROTECT_UNDERFLOW(m_stackTopFog);
        --m_stackTopFog;
        SetFog(m_stackFog[m_stackTopFog], false);
    };

    void CDevice::SetFog(bool enabled, bool force) {
        m_stackFog[m_stackTopFog] = enabled;

        DWORD state = enabled ? 1 : 0;
        if (m_curRenderState[D3DRS_FOGENABLE] != state) {
            ++m_stats.swRenderStates;
            _SetLastResult(m_pd3dDevice->SetRenderState(D3DRS_FOGENABLE, state));
            m_pd3dDevice->GetRenderState(D3DRS_FOGENABLE, (DWORD*)&m_curRenderState[D3DRS_FOGENABLE]);
        }
    };

    void CDevice::PushFogColor(unsigned int color) {
        bool changed = (m_stackFogColor[m_stackTopFogColor] != color);
        DuplicateFogColor();
        SetFogColor(color, changed);
    };

    void CDevice::DuplicateFogColor() {
        _PROTECT_OVERFLOW(m_stackTopFogColor);
        ++m_stackTopFogColor;
        m_stackFogColor[m_stackTopFogColor] = m_stackFogColor[m_stackTopFogColor - 1];
    };

    void CDevice::PopFogColor() {
        _PROTECT_UNDERFLOW(m_stackTopFogColor);
        --m_stackTopFogColor;
        SetFogColor(m_stackFogColor[m_stackTopFogColor], false);
    };

    void CDevice::SetFogColor(unsigned int color, bool force) {
        m_stackFogColor[m_stackTopFogColor] = color;

        if (m_curRenderState[D3DRS_FOGCOLOR] != color) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_FOGCOLOR] = color;
            m_pd3dDevice->SetRenderState(D3DRS_FOGCOLOR, color);
        }
    };

    void CDevice::PushFogMode(FogMode mode) {
        bool changed = (m_stackFogMode[m_stackTopFogMode] != mode);
        DuplicateFogMode();
        SetFogMode(mode, changed);
    };

    void CDevice::DuplicateFogMode() {
        _PROTECT_OVERFLOW(m_stackTopFogMode);
        ++m_stackTopFogMode;
        m_stackFogMode[m_stackTopFogMode] = m_stackFogMode[m_stackTopFogMode - 1];
    };

    void CDevice::PopFogMode() {
        _PROTECT_UNDERFLOW(m_stackTopFogMode);
        --m_stackTopFogMode;
        SetFogMode(m_stackFogMode[m_stackTopFogMode], false);
    };

    D3DFOGMODE m3dFogMToD3dFogM[4]{
        D3DFOG_NONE,
        D3DFOG_EXP,
        D3DFOG_EXP2,
        D3DFOG_LINEAR,
    };

    void CDevice::SetFogMode(FogMode mode, bool force) {
        m_stackFogMode[m_stackTopFogMode] = mode;

        D3DFOGMODE fogMode = m3dFogMToD3dFogM[mode];
        if (m_curRenderState[D3DRS_FOGVERTEXMODE] != fogMode) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_FOGVERTEXMODE] = fogMode;
            m_pd3dDevice->SetRenderState(D3DRS_FOGVERTEXMODE, fogMode);
        }
    };

    void CDevice::PushFogStart(float value) {
        bool changed = (m_stackFogStart[m_stackTopFogStart] != value);
        DuplicateFogStart();
        SetFogStart(value, changed);
    };

    void CDevice::DuplicateFogStart() {
        _PROTECT_OVERFLOW(m_stackTopFogStart);
        ++m_stackTopFogStart;
        m_stackFogStart[m_stackTopFogStart] = m_stackFogStart[m_stackTopFogStart - 1];
    };

    void CDevice::PopFogStart() {
        _PROTECT_UNDERFLOW(m_stackTopFogStart);
        --m_stackTopFogStart;
        SetFogStart(m_stackFogStart[m_stackTopFogStart], false);
    };

    void CDevice::SetFogStart(float value, bool force) {
        m_stackFogStart[m_stackTopFogStart] = value;

        if (*(float*)&m_curRenderState[D3DRS_FOGSTART] != value) {
            ++m_stats.swRenderStates;
            *(float*)&m_curRenderState[D3DRS_FOGSTART] = value;
            m_pd3dDevice->SetRenderState(D3DRS_FOGSTART, *(DWORD*)&value);
        }
    };

    void CDevice::PushFogEnd(float value) {
        bool changed = (m_stackFogEnd[m_stackTopFogEnd] != value);
        DuplicateFogEnd();
        SetFogEnd(value, changed);
    };

    void CDevice::DuplicateFogEnd() {
        _PROTECT_OVERFLOW(m_stackTopFogEnd);
        ++m_stackTopFogEnd;
        m_stackFogEnd[m_stackTopFogEnd] = m_stackFogEnd[m_stackTopFogEnd - 1];
    };

    void CDevice::PopFogEnd() {
        _PROTECT_UNDERFLOW(m_stackTopFogEnd);
        --m_stackTopFogEnd;
        SetFogEnd(m_stackFogEnd[m_stackTopFogEnd], false);
    };

    void CDevice::SetFogEnd(float value, bool force) {
        m_stackFogEnd[m_stackTopFogEnd] = value;

        if (*(float*)&m_curRenderState[D3DRS_FOGEND] != value) {
            ++m_stats.swRenderStates;
            *(float*)&m_curRenderState[D3DRS_FOGEND] = value;
            m_pd3dDevice->SetRenderState(D3DRS_FOGEND, *(DWORD*)&value);
        }
    };

    void CDevice::PushFillMode(FillMode mode) {
        bool changed = (m_stackFillMode[m_stackTopFillMode] != mode);
        DuplicateFillMode();
        SetFillMode(mode, changed);
    };

    void CDevice::DuplicateFillMode() {
        _PROTECT_OVERFLOW(m_stackTopFillMode);
        ++m_stackTopFillMode;
        m_stackFillMode[m_stackTopFillMode] = m_stackFillMode[m_stackTopFillMode - 1];
    };

    void CDevice::PopFillMode() {
        _PROTECT_UNDERFLOW(m_stackTopFillMode);
        --m_stackTopFillMode;
        SetFillMode(m_stackFillMode[m_stackTopFillMode], false);
    };

    static D3DFILLMODE m3dFmToD3dFm[3]{
        D3DFILL_POINT,
        D3DFILL_WIREFRAME,
        D3DFILL_SOLID,
    };

    void CDevice::SetFillMode(FillMode mode, bool force) {
        m_stackFillMode[m_stackTopFillMode] = mode;

        D3DFILLMODE fillMode = m3dFmToD3dFm[mode];
        if (m_curRenderState[D3DRS_FILLMODE] != fillMode) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_FILLMODE] = fillMode;
            m_pd3dDevice->SetRenderState(D3DRS_FILLMODE, fillMode);
        }
    };

    void CDevice::PushZBias(float value) {
        bool changed = (m_stackZBias[m_stackTopZBias] != value);
        DuplicateZBias();
        SetZBias(value, changed);
    };

    void CDevice::DuplicateZBias() {
        _PROTECT_OVERFLOW(m_stackTopZBias);
        ++m_stackTopZBias;
        m_stackZBias[m_stackTopZBias] = m_stackZBias[m_stackTopZBias - 1];
    };

    void CDevice::PopZBias() {
        _PROTECT_UNDERFLOW(m_stackTopZBias);
        --m_stackTopZBias;
        SetZBias(m_stackZBias[m_stackTopZBias], false);
    };

    void CDevice::SetZBias(float value, bool force) {
        m_stackZBias[m_stackTopZBias] = value;

        if (*(float*)&m_curRenderState[D3DRS_DEPTHBIAS] != value) {
            ++m_stats.swRenderStates;
            *(float*)&m_curRenderState[D3DRS_DEPTHBIAS] = value;
            m_pd3dDevice->SetRenderState(D3DRS_DEPTHBIAS, *(DWORD*)&value);
        }
    };

    void CDevice::PushZBiasSlopeScale(float value) {
        bool changed = (m_stackZBiasSlopeScale[m_stackTopZBiasSlopeScale] != value);
        DuplicateZBiasSlopeScale();
        SetZBiasSlopeScale(value, changed);
    };

    void CDevice::DuplicateZBiasSlopeScale() {
        _PROTECT_OVERFLOW(m_stackTopZBiasSlopeScale);
        ++m_stackTopZBiasSlopeScale;
        m_stackZBiasSlopeScale[m_stackTopZBiasSlopeScale] = m_stackZBiasSlopeScale[m_stackTopZBiasSlopeScale - 1];
    };

    void CDevice::PopZBiasSlopeScale() {
        _PROTECT_UNDERFLOW(m_stackTopZBiasSlopeScale);
        --m_stackTopZBiasSlopeScale;
        SetZBiasSlopeScale(m_stackZBiasSlopeScale[m_stackTopZBiasSlopeScale], false);
    };

    void CDevice::SetZBiasSlopeScale(float value, bool force) {
        m_stackZBiasSlopeScale[m_stackTopZBiasSlopeScale] = value;

        if (*(float*)&m_curRenderState[D3DRS_SLOPESCALEDEPTHBIAS] != value) {
            ++m_stats.swRenderStates;
            *(float*)&m_curRenderState[D3DRS_SLOPESCALEDEPTHBIAS] = value;
            m_pd3dDevice->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, *(DWORD*)&value);
        }
    };

    void CDevice::PushShadeMode(ShadeMode mode) {
        bool changed = (m_stackShadeMode[m_stackTopShadeMode] != mode);
        DuplicateShadeMode();
        SetShadeMode(mode, changed);
    };

    void CDevice::DuplicateShadeMode() {
        _PROTECT_OVERFLOW(m_stackTopShadeMode);
        ++m_stackTopShadeMode;
        m_stackShadeMode[m_stackTopShadeMode] = m_stackShadeMode[m_stackTopShadeMode - 1];
    };

    void CDevice::PopShadeMode() {
        _PROTECT_UNDERFLOW(m_stackTopShadeMode);
        --m_stackTopShadeMode;
        SetShadeMode(m_stackShadeMode[m_stackTopShadeMode], false);
    };

    D3DSHADEMODE m3dSmToD3dSm[2]{
        D3DSHADE_FLAT,
        D3DSHADE_GOURAUD,
    };

    void CDevice::SetShadeMode(ShadeMode mode, bool force) {
        m_stackShadeMode[m_stackTopShadeMode] = mode;

        D3DSHADEMODE shadeMode = m3dSmToD3dSm[mode];
        if (m_curRenderState[D3DRS_SHADEMODE] != shadeMode) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_SHADEMODE] = shadeMode;
            m_pd3dDevice->SetRenderState(D3DRS_SHADEMODE, shadeMode);
        }
    };

    void CDevice::PushPointSpriteEnable(bool enabled) {
        bool changed = (m_stackPointSpriteEnable[m_stackTopPointSpriteEnable] != enabled);
        DuplicatePointSpriteEnable();
        SetPointSpriteEnable(enabled, changed);
    };

    void CDevice::DuplicatePointSpriteEnable() {
        _PROTECT_OVERFLOW(m_stackTopPointSpriteEnable);
        ++m_stackTopPointSpriteEnable;
        m_stackPointSpriteEnable[m_stackTopPointSpriteEnable] = m_stackPointSpriteEnable[m_stackTopPointSpriteEnable - 1];
    };

    void CDevice::PopPointSpriteEnable() {
        _PROTECT_UNDERFLOW(m_stackTopPointSpriteEnable);
        --m_stackTopPointSpriteEnable;
        SetPointSpriteEnable(m_stackPointSpriteEnable[m_stackTopPointSpriteEnable], false);
    };

    void CDevice::SetPointSpriteEnable(bool enabled, bool force) {
        m_stackPointSpriteEnable[m_stackTopPointSpriteEnable] = enabled;

        if (m_curRenderState[D3DRS_POINTSPRITEENABLE] != enabled) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_POINTSPRITEENABLE] = enabled;
            m_pd3dDevice->SetRenderState(D3DRS_POINTSPRITEENABLE, enabled);
        }
    };

    void CDevice::PushPointScaleEnable(bool enabled) {
        bool changed = (m_stackPointScaleEnable[m_stackTopPointScaleEnable] != enabled);
        DuplicatePointScaleEnable();
        SetPointScaleEnable(enabled, changed);
    };

    void CDevice::DuplicatePointScaleEnable() {
        _PROTECT_OVERFLOW(m_stackTopPointScaleEnable);
        ++m_stackTopPointScaleEnable;
        m_stackPointScaleEnable[m_stackTopPointScaleEnable] = m_stackPointScaleEnable[m_stackTopPointScaleEnable - 1];
    };

    void CDevice::PopPointScaleEnable() {
        _PROTECT_UNDERFLOW(m_stackTopPointScaleEnable);
        --m_stackTopPointScaleEnable;
        SetPointScaleEnable(m_stackPointScaleEnable[m_stackTopPointScaleEnable], false);
    };

    void CDevice::SetPointScaleEnable(bool enabled, bool force) {
        m_stackPointScaleEnable[m_stackTopPointScaleEnable] = enabled;

        if (m_curRenderState[D3DRS_POINTSCALEENABLE] != enabled) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_POINTSCALEENABLE] = enabled;
            m_pd3dDevice->SetRenderState(D3DRS_POINTSCALEENABLE, enabled);
        }
    };

    void CDevice::PushPointSizeMin(float value) {
        bool changed = (m_stackPointSizeMin[m_stackTopPointSizeMin] != value);
        DuplicatePointSizeMin();
        SetPointSizeMin(value, changed);
    };

    void CDevice::DuplicatePointSizeMin() {
        _PROTECT_OVERFLOW(m_stackTopPointSizeMin);
        ++m_stackTopPointSizeMin;
        m_stackPointSizeMin[m_stackTopPointSizeMin] = m_stackPointSizeMin[m_stackTopPointSizeMin - 1];
    };

    void CDevice::PopPointSizeMin() {
        _PROTECT_UNDERFLOW(m_stackTopPointSizeMin);
        --m_stackTopPointSizeMin;
        SetPointSizeMin(m_stackPointSizeMin[m_stackTopPointSizeMin], false);
    };

    void CDevice::SetPointSizeMin(float value, bool force) {
        m_stackPointSizeMin[m_stackTopPointSizeMin] = value;

        if (*(float*)&m_curRenderState[D3DRS_POINTSIZE_MIN] != value) {
            ++m_stats.swRenderStates;
            *(float*)&m_curRenderState[D3DRS_POINTSIZE_MIN] = value;
            m_pd3dDevice->SetRenderState(D3DRS_POINTSIZE_MIN, *(DWORD*)&value);
        }
    };

    void CDevice::PushPointSizeMax(float value) {
        bool changed = (m_stackPointSizeMax[m_stackTopPointSizeMax] != value);
        DuplicatePointSizeMax();
        SetPointSizeMax(value, changed);
    };

    void CDevice::DuplicatePointSizeMax() {
        _PROTECT_OVERFLOW(m_stackTopPointSizeMax);
        ++m_stackTopPointSizeMax;
        m_stackPointSizeMax[m_stackTopPointSizeMax] = m_stackPointSizeMax[m_stackTopPointSizeMax - 1];
    };

    void CDevice::PopPointSizeMax() {
        _PROTECT_UNDERFLOW(m_stackTopPointSizeMax);
        --m_stackTopPointSizeMax;
        SetPointSizeMax(m_stackPointSizeMax[m_stackTopPointSizeMax], false);
    };

    void CDevice::SetPointSizeMax(float value, bool force) {
        m_stackPointSizeMax[m_stackTopPointSizeMax] = value;

        if (*(float*)&m_curRenderState[D3DRS_POINTSIZE_MAX] != value) {
            ++m_stats.swRenderStates;
            *(float*)&m_curRenderState[D3DRS_POINTSIZE_MAX] = value;
            m_pd3dDevice->SetRenderState(D3DRS_POINTSIZE_MAX, *(DWORD*)&value);
        }
    };

    void CDevice::PushPointSize(float value) {
        bool changed = (m_stackPointSize[m_stackTopPointSize] != value);
        DuplicatePointSize();
        SetPointSize(value, changed);
    };

    void CDevice::DuplicatePointSize() {
        _PROTECT_OVERFLOW(m_stackTopPointSize);
        ++m_stackTopPointSize;
        m_stackPointSize[m_stackTopPointSize] = m_stackPointSize[m_stackTopPointSize - 1];
    };

    void CDevice::PopPointSize() {
        _PROTECT_UNDERFLOW(m_stackTopPointSize);
        --m_stackTopPointSize;
        SetPointSize(m_stackPointSize[m_stackTopPointSize], false);
    };

    void CDevice::SetPointSize(float value, bool force) {
        m_stackPointSize[m_stackTopPointSize] = value;

        if (*(float*)&m_curRenderState[D3DRS_POINTSIZE] != value) {
            ++m_stats.swRenderStates;
            *(float*)&m_curRenderState[D3DRS_POINTSIZE] = value;
            m_pd3dDevice->SetRenderState(D3DRS_POINTSIZE, *(DWORD*)&value);
        }
    };

    void CDevice::PushPointScaleA(float value) {
        bool changed = (m_stackPointScaleA[m_stackTopPointScaleA] != value);
        DuplicatePointScaleA();
        SetPointScaleA(value, changed);
    };

    void CDevice::DuplicatePointScaleA() {
        _PROTECT_OVERFLOW(m_stackTopPointScaleA);
        ++m_stackTopPointScaleA;
        m_stackPointScaleA[m_stackTopPointScaleA] = m_stackPointScaleA[m_stackTopPointScaleA - 1];
    };

    void CDevice::PopPointScaleA() {
        _PROTECT_UNDERFLOW(m_stackTopPointScaleA);
        --m_stackTopPointScaleA;
        SetPointScaleA(m_stackPointScaleA[m_stackTopPointScaleA], false);
    };

    void CDevice::SetPointScaleA(float value, bool force) {
        m_stackPointScaleA[m_stackTopPointScaleA] = value;

        if (*(float*)&m_curRenderState[D3DRS_POINTSCALE_A] != value) {
            ++m_stats.swRenderStates;
            *(float*)&m_curRenderState[D3DRS_POINTSCALE_A] = value;
            m_pd3dDevice->SetRenderState(D3DRS_POINTSCALE_A, *(DWORD*)&value);
        }
    };

    void CDevice::PushPointScaleB(float value) {
        bool changed = (m_stackPointScaleB[m_stackTopPointScaleB] != value);
        DuplicatePointScaleB();
        SetPointScaleB(value, changed);
    };

    void CDevice::DuplicatePointScaleB() {
        _PROTECT_OVERFLOW(m_stackTopPointScaleB);
        ++m_stackTopPointScaleB;
        m_stackPointScaleB[m_stackTopPointScaleB] = m_stackPointScaleB[m_stackTopPointScaleB - 1];
    };

    void CDevice::PopPointScaleB() {
        _PROTECT_UNDERFLOW(m_stackTopPointScaleB);
        --m_stackTopPointScaleB;
        SetPointScaleB(m_stackPointScaleB[m_stackTopPointScaleB], false);
    };

    void CDevice::SetPointScaleB(float value, bool force) {
        m_stackPointScaleB[m_stackTopPointScaleB] = value;

        if (*(float*)&m_curRenderState[D3DRS_POINTSCALE_B] != value) {
            ++m_stats.swRenderStates;
            *(float*)&m_curRenderState[D3DRS_POINTSCALE_B] = value;
            m_pd3dDevice->SetRenderState(D3DRS_POINTSCALE_B, *(DWORD*)&value);
        }
    };

    void CDevice::PushPointScaleC(float value) {
        bool changed = (m_stackPointScaleC[m_stackTopPointScaleC] != value);
        DuplicatePointScaleC();
        SetPointScaleC(value, changed);
    };

    void CDevice::DuplicatePointScaleC() {
        _PROTECT_OVERFLOW(m_stackTopPointScaleC);
        ++m_stackTopPointScaleC;
        m_stackPointScaleC[m_stackTopPointScaleC] = m_stackPointScaleC[m_stackTopPointScaleC - 1];
    };

    void CDevice::PopPointScaleC() {
        _PROTECT_UNDERFLOW(m_stackTopPointScaleC);
        --m_stackTopPointScaleC;
        SetPointScaleC(m_stackPointScaleC[m_stackTopPointScaleC], false);
    };

    void CDevice::SetPointScaleC(float value, bool force) {
        m_stackPointScaleC[m_stackTopPointScaleC] = value;

        if (*(float*)&m_curRenderState[D3DRS_POINTSCALE_C] != value) {
            ++m_stats.swRenderStates;
            *(float*)&m_curRenderState[D3DRS_POINTSCALE_C] = value;
            m_pd3dDevice->SetRenderState(D3DRS_POINTSCALE_C, *(DWORD*)&value);
        }
    };

    void CDevice::PushTFactor(unsigned int color) {
        bool changed = (m_stackTFactor[m_stackTopTFactor] != color);
        DuplicateTFactor();
        SetTFactor(color, changed);
    };

    void CDevice::DuplicateTFactor() {
        ++m_stackTopTFactor;
        m_stackTFactor[m_stackTopTFactor] = m_stackTFactor[m_stackTopTFactor - 1];
    };

    void CDevice::PopTFactor() {
        _PROTECT_UNDERFLOW(m_stackTopTFactor);
        --m_stackTopTFactor;
        SetTFactor(m_stackTFactor[m_stackTopTFactor], false);
    };

    void CDevice::SetTFactor(unsigned int color, bool force) {
        m_stackTFactor[m_stackTopTFactor] = color;

        if (m_curRenderState[D3DRS_TEXTUREFACTOR] != color) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_TEXTUREFACTOR] = color;
            m_pd3dDevice->SetRenderState(D3DRS_TEXTUREFACTOR, color);
        }
    };

    void CDevice::PushLocalViewer(bool enabled) {
        bool changed = (m_stackLocalViewer[m_stackTopLocalViewer] != enabled);
        DuplicateLocalViewer();
        SetLocalViewer(enabled, changed);
    };

    void CDevice::DuplicateLocalViewer() {
        _PROTECT_OVERFLOW(m_stackTopLocalViewer);
        ++m_stackTopLocalViewer;
        m_stackLocalViewer[m_stackTopLocalViewer] = m_stackLocalViewer[m_stackTopLocalViewer - 1];
    };

    void CDevice::PopLocalViewer() {
        _PROTECT_UNDERFLOW(m_stackTopLocalViewer);
        --m_stackTopLocalViewer;
        SetLocalViewer(m_stackLocalViewer[m_stackTopLocalViewer], false);
    };

    void CDevice::SetLocalViewer(bool enabled, bool force) {
        m_stackLocalViewer[m_stackTopLocalViewer] = enabled;

        if (m_curRenderState[D3DRS_LOCALVIEWER] != enabled) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_LOCALVIEWER] = enabled;
            m_pd3dDevice->SetRenderState(D3DRS_LOCALVIEWER, enabled);
        }
    };

    void CDevice::PushSpecularLighting(bool enabled) {
        bool changed = (m_stackSpecularLighting[m_stackTopSpecularLighting] != enabled);
        DuplicateSpecularLighting();
        SetSpecularLighting(enabled, changed);
    };

    void CDevice::DuplicateSpecularLighting() {
        _PROTECT_OVERFLOW(m_stackTopSpecularLighting);
        ++m_stackTopSpecularLighting;
        m_stackSpecularLighting[m_stackTopSpecularLighting] = m_stackSpecularLighting[m_stackTopSpecularLighting - 1];
    };

    void CDevice::PopSpecularLighting() {
        _PROTECT_UNDERFLOW(m_stackTopSpecularLighting);
        --m_stackTopSpecularLighting;
        SetSpecularLighting(m_stackSpecularLighting[m_stackTopSpecularLighting], false);
    };

    void CDevice::SetSpecularLighting(bool enabled, bool force) {
        m_stackSpecularLighting[m_stackTopSpecularLighting] = enabled;

        if (m_curRenderState[D3DRS_SPECULARENABLE] != enabled) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_SPECULARENABLE] = enabled;
            m_pd3dDevice->SetRenderState(D3DRS_SPECULARENABLE, enabled);
        }
    };

    void CDevice::PushColorWriteMask(unsigned int mask) {
        bool changed = (m_stackColorWriteMask[m_stackTopColorWriteMask] != mask);
        DuplicateColorWriteMask();
        SetColorWriteMask(mask, changed);
    };

    void CDevice::DuplicateColorWriteMask() {
        _PROTECT_OVERFLOW(m_stackTopColorWriteMask);
        ++m_stackTopColorWriteMask;
        m_stackColorWriteMask[m_stackTopColorWriteMask] = m_stackColorWriteMask[m_stackTopColorWriteMask - 1];
    };

    void CDevice::PopColorWriteMask() {
        _PROTECT_UNDERFLOW(m_stackTopColorWriteMask);
        --m_stackTopColorWriteMask;
        SetColorWriteMask(m_stackColorWriteMask[m_stackTopColorWriteMask], false);
    };

    void CDevice::SetColorWriteMask(unsigned int mask, bool force) {
        m_stackColorWriteMask[m_stackTopColorWriteMask] = mask;

        if (m_curRenderState[D3DRS_COLORWRITEENABLE] != mask) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_COLORWRITEENABLE] = mask;
            m_pd3dDevice->SetRenderState(D3DRS_COLORWRITEENABLE, mask);
        }
    };

    void CDevice::PushDithering(bool enabled) {
        bool changed = (m_stackDithering[m_stackTopDithering] != enabled);
        DuplicateDithering();
        SetDithering(enabled, changed);
    };

    void CDevice::DuplicateDithering() {
        _PROTECT_OVERFLOW(m_stackTopDithering);
        ++m_stackTopDithering;
        m_stackDithering[m_stackTopDithering] = m_stackDithering[m_stackTopDithering - 1];
    };

    void CDevice::PopDithering() {
        _PROTECT_UNDERFLOW(m_stackTopDithering);
        --m_stackTopDithering;
        SetDithering(m_stackDithering[m_stackTopDithering], false);
    };

    void CDevice::SetDithering(bool enabled, bool force) {
        m_stackDithering[m_stackTopDithering] = enabled;

        if (m_curRenderState[D3DRS_DITHERENABLE] != enabled) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_DITHERENABLE] = enabled;
            m_pd3dDevice->SetRenderState(D3DRS_DITHERENABLE, enabled);
        }
    };

    void CDevice::PushNPatchLevel(float value) {
        bool changed = (m_stackNPatchLevel[m_stackTopNPatchLevel] != value);
        DuplicateNPatchLevel();
        SetNPatchLevel(value, changed);
    };

    void CDevice::DuplicateNPatchLevel() {
        _PROTECT_OVERFLOW(m_stackTopNPatchLevel);
        ++m_stackTopNPatchLevel;
        m_stackNPatchLevel[m_stackTopNPatchLevel] = m_stackNPatchLevel[m_stackTopNPatchLevel - 1];
    };

    void CDevice::PopNPatchLevel() {
        _PROTECT_UNDERFLOW(m_stackTopNPatchLevel);
        --m_stackTopNPatchLevel;
        SetNPatchLevel(m_stackNPatchLevel[m_stackTopNPatchLevel], false);
    };

    void CDevice::SetNPatchLevel(float value, bool force) {
        m_stackNPatchLevel[m_stackTopNPatchLevel] = value;
        m_pd3dDevice->SetNPatchMode(value);
    };

    void CDevice::PushStencilState(bool enable) {
        bool changed = (m_stackStencilState[m_stackTopStencilState] != enable);
        DuplicateStencilState();
        SetStencilState(enable, changed);
    };

    void CDevice::DuplicateStencilState() {
        _PROTECT_OVERFLOW(m_stackTopStencilState);
        ++m_stackTopStencilState;
        m_stackStencilState[m_stackTopStencilState] = m_stackStencilState[m_stackTopStencilState - 1];
    };

    void CDevice::PopStencilState() {
        _PROTECT_UNDERFLOW(m_stackTopStencilState);
        --m_stackTopStencilState;
        SetStencilState(m_stackStencilState[m_stackTopStencilState], false);
    };

    void CDevice::SetStencilState(bool enabled, bool force) {
        m_stackStencilState[m_stackTopStencilState] = enabled;

        if (m_curRenderState[D3DRS_STENCILENABLE] != enabled) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_STENCILENABLE] = enabled;
            m_pd3dDevice->SetRenderState(D3DRS_STENCILENABLE, enabled);
        }

        m_stencilLevel[m_activeStencilTarget] = -1;
    };

    void CDevice::PushStencilMask(unsigned int mask) {
        bool changed = (m_stackStencilMask[m_stackTopStencilMask] != mask);
        DuplicateStencilMask();
        SetStencilMask(mask, changed);
    };

    void CDevice::DuplicateStencilMask() {
        _PROTECT_OVERFLOW(m_stackTopStencilMask);
        ++m_stackTopStencilMask;
        m_stackStencilMask[m_stackTopStencilMask] = m_stackStencilMask[m_stackTopStencilMask - 1];
    };

    void CDevice::PopStencilMask() {
        _PROTECT_UNDERFLOW(m_stackTopStencilMask);
        --m_stackTopStencilMask;
        SetStencilMask(m_stackStencilMask[m_stackTopStencilMask], false);
    };

    void CDevice::SetStencilMask(unsigned int mask, bool force) {
        m_stackStencilMask[m_stackTopStencilMask] = mask;

        if (m_curRenderState[D3DRS_STENCILMASK] != mask) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_STENCILMASK] = mask;
            m_pd3dDevice->SetRenderState(D3DRS_STENCILMASK, mask);
        }
    };

    void CDevice::PushStencilRef(uint32_t ref) {
        bool changed = (m_stackStencilRef[m_stackTopStencilRef] != ref);
        DuplicateStencilRef();
        SetStencilRef(ref, changed);
    };

    void CDevice::DuplicateStencilRef() {
        _PROTECT_OVERFLOW(m_stackTopStencilRef);
        ++m_stackTopStencilRef;
        m_stackStencilRef[m_stackTopStencilRef] = m_stackStencilRef[m_stackTopStencilRef - 1];
    };

    void CDevice::PopStencilRef() {
        _PROTECT_UNDERFLOW(m_stackTopStencilRef);
        --m_stackTopStencilRef;
        SetStencilRef(m_stackStencilRef[m_stackTopStencilRef], false);
    };

    void CDevice::SetStencilRef(unsigned int ref, bool force) {
        m_stackStencilRef[m_stackTopStencilRef] = ref;

        if (m_curRenderState[D3DRS_STENCILREF] != ref) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_STENCILREF] = ref;
            m_pd3dDevice->SetRenderState(D3DRS_STENCILREF, ref);
        }
    };

    void CDevice::PushStencilWriteMask(unsigned int mask) {
        bool changed = (m_stackStencilWriteMask[m_stackTopStencilWriteMask] != mask);
        DuplicateStencilWriteMask();
        SetStencilWriteMask(mask, changed);
    };

    void CDevice::DuplicateStencilWriteMask() {
        _PROTECT_OVERFLOW(m_stackTopStencilWriteMask);
        ++m_stackTopStencilWriteMask;
        m_stackStencilWriteMask[m_stackTopStencilWriteMask] = m_stackStencilWriteMask[m_stackTopStencilWriteMask - 1];
    };

    void CDevice::PopStencilWriteMask() {
        _PROTECT_UNDERFLOW(m_stackTopStencilWriteMask);
        --m_stackTopStencilWriteMask;
        SetStencilWriteMask(m_stackStencilWriteMask[m_stackTopStencilWriteMask], false);
    };

    void CDevice::SetStencilWriteMask(uint32_t mask, bool force) {
        m_stackStencilWriteMask[m_stackTopStencilWriteMask] = mask;

        if (m_curRenderState[D3DRS_STENCILWRITEMASK] != mask) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_STENCILWRITEMASK] = mask;
            m_pd3dDevice->SetRenderState(D3DRS_STENCILWRITEMASK, mask);
        }
    };

    void CDevice::PushStencilFunc(CmpFunc func) {
        bool changed = (m_stackStencilFunc[m_stackTopStencilFunc] != func);
        DuplicateStencilFunc();
        SetStencilFunc(func, changed);
    };

    void CDevice::DuplicateStencilFunc() {
        _PROTECT_OVERFLOW(m_stackTopStencilFunc);
        ++m_stackTopStencilFunc;
        m_stackStencilFunc[m_stackTopStencilFunc] = m_stackStencilFunc[m_stackTopStencilFunc - 1];
    };

    void CDevice::PopStencilFunc() {
        _PROTECT_UNDERFLOW(m_stackTopStencilFunc);
        --m_stackTopStencilFunc;
        SetStencilFunc(m_stackStencilFunc[m_stackTopStencilFunc], false);
    };

    void CDevice::SetStencilFunc(CmpFunc func, bool force) {
        m_stackStencilFunc[m_stackTopStencilFunc] = func;

        D3DCMPFUNC cmpFunc = m3dCmpToD3dCmp[func];
        if (m_curRenderState[D3DRS_STENCILFUNC] != cmpFunc) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_STENCILFUNC] = cmpFunc;
            m_pd3dDevice->SetRenderState(D3DRS_STENCILFUNC, cmpFunc);
        }
    };

    void CDevice::PushStencilFail(StencilOp op) {
        bool changed = (m_stackStencilFail[m_stackTopStencilFail] != op);
        DuplicateStencilFail();
        SetStencilFail(op, changed);
    };

    void CDevice::DuplicateStencilFail() {
        _PROTECT_OVERFLOW(m_stackTopStencilFail);
        ++m_stackTopStencilFail;
        m_stackStencilFail[m_stackTopStencilFail] = m_stackStencilFail[m_stackTopStencilFail - 1];
    };

    void CDevice::PopStencilFail() {
        _PROTECT_UNDERFLOW(m_stackTopStencilFail);
        --m_stackTopStencilFail;
        SetStencilFail(m_stackStencilFail[m_stackTopStencilFail], false);
    };

    static D3DSTENCILOP m3dStencilOpD3dStencilOp[8]{
        D3DSTENCILOP_KEEP,
        D3DSTENCILOP_ZERO,
        D3DSTENCILOP_REPLACE,
        D3DSTENCILOP_INCRSAT,
        D3DSTENCILOP_DECRSAT,
        D3DSTENCILOP_INVERT,
        D3DSTENCILOP_INCR,
        D3DSTENCILOP_DECR,
    };

    void CDevice::SetStencilFail(StencilOp op, bool force) {
        m_stackStencilFail[m_stackTopStencilFail] = op;

        D3DSTENCILOP stencilOp = m3dStencilOpD3dStencilOp[op];
        if (m_curRenderState[D3DRS_STENCILFAIL] != stencilOp) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_STENCILFAIL] = stencilOp;
            m_pd3dDevice->SetRenderState(D3DRS_STENCILFAIL, stencilOp);
        }
    };

    void CDevice::PushStencilZFail(StencilOp op) {
        bool changed = (m_stackStencilZFail[m_stackTopStencilZFail] != op);
        DuplicateStencilZFail();
        SetStencilZFail(op, changed);
    };

    void CDevice::DuplicateStencilZFail() {
        _PROTECT_OVERFLOW(m_stackTopStencilZFail);
        ++m_stackTopStencilZFail;
        m_stackStencilZFail[m_stackTopStencilZFail] = m_stackStencilZFail[m_stackTopStencilZFail - 1];
    };

    void CDevice::PopStencilZFail() {
        _PROTECT_UNDERFLOW(m_stackTopStencilZFail);
        --m_stackTopStencilZFail;
        SetStencilZFail(m_stackStencilZFail[m_stackTopStencilZFail], false);
    };

    void CDevice::SetStencilZFail(StencilOp op, bool force) {
        m_stackStencilZFail[m_stackTopStencilZFail] = op;

        D3DSTENCILOP stencilOp = m3dStencilOpD3dStencilOp[op];
        if (m_curRenderState[D3DRS_STENCILZFAIL] != stencilOp) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_STENCILZFAIL] = stencilOp;
            m_pd3dDevice->SetRenderState(D3DRS_STENCILZFAIL, stencilOp);
        }
    };

    void CDevice::PushStencilPass(StencilOp op) {
        bool changed = (m_stackStencilPass[m_stackTopStencilPass] != op);
        DuplicateStencilPass();
        SetStencilPass(op, changed);
    };

    void CDevice::DuplicateStencilPass() {
        _PROTECT_OVERFLOW(m_stackTopStencilPass);
        ++m_stackTopStencilPass;
        m_stackStencilPass[m_stackTopStencilPass] = m_stackStencilPass[m_stackTopStencilPass - 1];
    };

    void CDevice::PopStencilPass() {
        _PROTECT_UNDERFLOW(m_stackTopStencilPass);
        --m_stackTopStencilPass;
        SetStencilPass(m_stackStencilPass[m_stackTopStencilPass], false);
    };

    void CDevice::SetStencilPass(StencilOp op, bool force) {
        m_stackStencilPass[m_stackTopStencilPass] = op;

        D3DSTENCILOP stencilOp = m3dStencilOpD3dStencilOp[op];
        if (m_curRenderState[D3DRS_STENCILPASS] != stencilOp) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_STENCILPASS] = stencilOp;
            m_pd3dDevice->SetRenderState(D3DRS_STENCILPASS, stencilOp);
        }
    };

    void CDevice::PushStencil2SidedEnable(bool enabled) {
        bool changed = (m_stackStencil2SidedEnable[m_stackTopStencil2SidedEnable] != enabled);
        DuplicateStencil2SidedEnable();
        SetStencil2SidedEnable(enabled, changed);
    };

    void CDevice::DuplicateStencil2SidedEnable() {
        _PROTECT_OVERFLOW(m_stackTopStencil2SidedEnable);
        ++m_stackTopStencil2SidedEnable;
        m_stackStencil2SidedEnable[m_stackTopStencil2SidedEnable] = m_stackStencil2SidedEnable[m_stackTopStencil2SidedEnable - 1];
    };

    void CDevice::PopStencil2SidedEnable() {
        _PROTECT_UNDERFLOW(m_stackTopStencil2SidedEnable);
        --m_stackTopStencil2SidedEnable;
        SetStencil2SidedEnable(m_stackStencil2SidedEnable[m_stackTopStencil2SidedEnable], false);
    };

    void CDevice::SetStencil2SidedEnable(bool enabled, bool force) {
        m_stackStencil2SidedEnable[m_stackTopStencil2SidedEnable] = enabled;

        if (m_curRenderState[D3DRS_TWOSIDEDSTENCILMODE] != enabled) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_TWOSIDEDSTENCILMODE] = enabled;
            m_pd3dDevice->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, enabled);
        }

        m_stencilLevel[m_activeStencilTarget] = -1;
    };

    void CDevice::PushStencilCcwFunc(CmpFunc func) {
        bool changed = (m_stackStencilCcwFunc[m_stackTopStencilCcwFunc] != func);
        DuplicateStencilCcwFunc();
        SetStencilCcwFunc(func, changed);
    };

    void CDevice::DuplicateStencilCcwFunc() {
        _PROTECT_OVERFLOW(m_stackTopStencilCcwFunc);
        ++m_stackTopStencilCcwFunc;
        m_stackStencilCcwFunc[m_stackTopStencilCcwFunc] = m_stackStencilCcwFunc[m_stackTopStencilCcwFunc - 1];
    };

    void CDevice::PopStencilCcwFunc() {
        _PROTECT_UNDERFLOW(m_stackTopStencilCcwFunc);
        --m_stackTopStencilCcwFunc;
        SetStencilCcwFunc(m_stackStencilCcwFunc[m_stackTopStencilCcwFunc], false);
    };

    void CDevice::SetStencilCcwFunc(CmpFunc func, bool force) {
        m_stackStencilCcwFunc[m_stackTopStencilCcwFunc] = func;

        D3DCMPFUNC cmpFunc = m3dCmpToD3dCmp[func];
        if (m_curRenderState[D3DRS_CCW_STENCILFUNC] != cmpFunc) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_CCW_STENCILFUNC] = cmpFunc;
            m_pd3dDevice->SetRenderState(D3DRS_CCW_STENCILFUNC, cmpFunc);
        }
    };

    void CDevice::PushStencilCcwFail(StencilOp op) {
        bool changed = (m_stackStencilCcwFail[m_stackTopStencilCcwFail] != op);
        DuplicateStencilCcwFail();
        SetStencilCcwFail(op, changed);
    };

    void CDevice::DuplicateStencilCcwFail() {
        _PROTECT_OVERFLOW(m_stackTopStencilCcwFail);
        ++m_stackTopStencilCcwFail;
        m_stackStencilCcwFail[m_stackTopStencilCcwFail] = m_stackStencilCcwFail[m_stackTopStencilCcwFail - 1];
    };

    void CDevice::PopStencilCcwFail() {
        _PROTECT_UNDERFLOW(m_stackTopStencilCcwFail);
        --m_stackTopStencilCcwFail;
        SetStencilCcwFail(m_stackStencilCcwFail[m_stackTopStencilCcwFail], false);
    };

    void CDevice::SetStencilCcwFail(StencilOp op, bool force) {
        m_stackStencilCcwFail[m_stackTopStencilCcwFail] = op;

        D3DSTENCILOP stencilOp = m3dStencilOpD3dStencilOp[op];
        if (m_curRenderState[D3DRS_CCW_STENCILFAIL] != stencilOp) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_CCW_STENCILFAIL] = stencilOp;
            m_pd3dDevice->SetRenderState(D3DRS_CCW_STENCILFAIL, stencilOp);
        }
    };

    void CDevice::PushStencilCcwZFail(StencilOp op) {
        bool changed = (m_stackStencilCcwZFail[m_stackTopStencilCcwZFail] != op);
        DuplicateStencilCcwZFail();
        SetStencilCcwZFail(op, changed);
    };

    void CDevice::DuplicateStencilCcwZFail() {
        _PROTECT_OVERFLOW(m_stackTopStencilCcwZFail);
        ++m_stackTopStencilCcwZFail;
        m_stackStencilCcwZFail[m_stackTopStencilCcwZFail] = m_stackStencilCcwZFail[m_stackTopStencilCcwZFail - 1];
    };

    void CDevice::PopStencilCcwZFail() {
        _PROTECT_UNDERFLOW(m_stackTopStencilCcwZFail);
        --m_stackTopStencilCcwZFail;
        SetStencilCcwZFail(m_stackStencilCcwZFail[m_stackTopStencilCcwZFail], false);
    };

    void CDevice::SetStencilCcwZFail(StencilOp op, bool force) {
        m_stackStencilCcwZFail[m_stackTopStencilCcwZFail] = op;

        D3DSTENCILOP stencilOp = m3dStencilOpD3dStencilOp[op];
        if (m_curRenderState[D3DRS_CCW_STENCILZFAIL] != stencilOp) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_CCW_STENCILZFAIL] = stencilOp;
            m_pd3dDevice->SetRenderState(D3DRS_CCW_STENCILZFAIL, stencilOp);
        }
    };

    void CDevice::PushStencilCcwPass(StencilOp op) {
        bool changed = (m_stackStencilCcwPass[m_stackTopStencilCcwPass] != op);
        DuplicateStencilCcwPass();
        SetStencilCcwPass(op, changed);
    };

    void CDevice::DuplicateStencilCcwPass() {
        _PROTECT_OVERFLOW(m_stackTopStencilCcwPass);
        ++m_stackTopStencilCcwPass;
        m_stackStencilCcwPass[m_stackTopStencilCcwPass] = m_stackStencilCcwPass[m_stackTopStencilCcwPass - 1];
    };

    void CDevice::PopStencilCcwPass() {
        _PROTECT_UNDERFLOW(m_stackTopStencilCcwPass);
        --m_stackTopStencilCcwPass;
        SetStencilCcwPass(m_stackStencilCcwPass[m_stackTopStencilCcwPass], false);
    };

    void CDevice::SetStencilCcwPass(StencilOp op, bool force) {
        m_stackStencilCcwPass[m_stackTopStencilCcwPass] = op;

        D3DSTENCILOP stencilOp = m3dStencilOpD3dStencilOp[op];
        if (m_curRenderState[D3DRS_CCW_STENCILPASS] != stencilOp) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_CCW_STENCILPASS] = stencilOp;
            m_pd3dDevice->SetRenderState(D3DRS_CCW_STENCILPASS, stencilOp);
        }
    };

    void CDevice::PushMultiSample(int32_t enabled) {
        bool changed = (m_stackMultiSample[m_stackTopMultiSample] != enabled);
        DuplicateMultiSample();
        SetMultiSample(enabled, changed);
    };

    void CDevice::DuplicateMultiSample() {
        _PROTECT_OVERFLOW(m_stackTopMultiSample);
        ++m_stackTopMultiSample;
        m_stackMultiSample[m_stackTopMultiSample] = m_stackMultiSample[m_stackTopMultiSample - 1];
    };

    void CDevice::PopMultiSample() {
        _PROTECT_UNDERFLOW(m_stackTopMultiSample);
        --m_stackTopMultiSample;
        SetMultiSample(m_stackMultiSample[m_stackTopMultiSample], false);
    };

    void CDevice::SetMultiSample(int32_t enabled, bool force) {
        m_stackMultiSample[m_stackTopMultiSample] = enabled;

        if (m_d3dpp.MultiSampleType != D3DMULTISAMPLE_NONE) {
            if (m_curRenderState[D3DRS_MULTISAMPLEANTIALIAS] != enabled) {
                ++m_stats.swRenderStates;
                m_curRenderState[D3DRS_MULTISAMPLEANTIALIAS] = enabled;
                m_pd3dDevice->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, enabled);
            }
        }
    };

    void CDevice::PushMultiSampleMask(uint32_t mask) {
        bool changed = (m_stackMultiSampleMask[m_stackTopMultiSampleMask] != mask);
        DuplicateMultiSampleMask();
        SetMultiSampleMask(mask, changed);
    };

    void CDevice::DuplicateMultiSampleMask() {
        _PROTECT_OVERFLOW(m_stackTopMultiSampleMask);
        ++m_stackTopMultiSampleMask;
        m_stackMultiSampleMask[m_stackTopMultiSampleMask] = m_stackMultiSampleMask[m_stackTopMultiSampleMask - 1];
    };

    void CDevice::PopMultiSampleMask() {
        _PROTECT_UNDERFLOW(m_stackTopMultiSampleMask);
        --m_stackTopMultiSampleMask;
        SetMultiSampleMask(m_stackMultiSampleMask[m_stackTopMultiSampleMask], false);
    };

    void CDevice::SetMultiSampleMask(uint32_t mask, bool force) {
        m_stackMultiSampleMask[m_stackTopMultiSampleMask] = mask;

        if (m_curRenderState[D3DRS_MULTISAMPLEMASK] != mask) {
            ++m_stats.swRenderStates;
            m_curRenderState[D3DRS_MULTISAMPLEMASK] = mask;
            m_pd3dDevice->SetRenderState(D3DRS_MULTISAMPLEMASK, mask);
        }
    };

    void CDevice::SetViewMatrix(const hta::CMatrix& viewMatrix) {
        m_viewMatrix    = viewMatrix;
        m_viewMatrixInv = ~m_viewMatrix;

        m_viewOrigin = m_viewMatrixInv.GetOrigin();

        m_viewMatrixWasSetThisFrame = true;
        ForceRecalcClipPlanes();
        m_updateModelMatrix = true;
    };

    const hta::CMatrix& CDevice::GetViewMatrix() const {
        return m_viewMatrix;
    };

    const hta::CMatrix& CDevice::GetInvViewMatrix() const {
        return m_viewMatrixInv;
    };

    const hta::CVector& CDevice::GetViewOrigin() const {
        return m_viewOrigin;
    };

    void CDevice::MatPush(const hta::CMatrix& mat) {
        MatDuplicate();
        MatMul(mat);
    };

    void CDevice::MatDuplicate() {
        if (m_matViewStackTop < 63) {
            m_matViewStackTop++;
        }

        m_matViewStack[m_matViewStackTop] = m_matViewStack[m_matViewStackTop - 1];

        m_matViewIsNotActuated = true;
        m_updateModelViewProj  = true;
        m_updateModelMatrix    = true;
    };

    void CDevice::MatPop(bool dontset) {
        if (m_matViewStackTop > 0) {
            m_matViewStackTop--;
        }

        m_matInvViewIsNotActuated = true;
        m_matViewIsNotActuated    = true;
        m_updateModelViewProj     = true;
        m_updateModelMatrix       = true;
    };

    void CDevice::MatMul(const hta::CMatrix& mat) {
        m_matViewStack[m_matViewStackTop] = mat * m_matViewStack[m_matViewStackTop];

        m_matInvViewIsNotActuated = true;
        m_matViewIsNotActuated    = true;
        m_updateModelViewProj     = true;
        m_updateModelMatrix       = true;
    };

    void CDevice::MatMulR(const hta::CMatrix& mat) {
        m_matViewStack[m_matViewStackTop] = m_matViewStack[m_matViewStackTop] * mat;

        m_matInvViewIsNotActuated = true;
        m_matViewIsNotActuated    = true;
        m_updateModelViewProj     = true;
        m_updateModelMatrix       = true;
    };

    const hta::CMatrix& CDevice::MatGet() const {
        return m_matViewStack[m_matViewStackTop];
    };

    const hta::CMatrix& CDevice::MatGetInv() {
        if (m_matInvViewIsNotActuated) {
            m_matInvView              = ~m_matViewStack[m_matViewStackTop];
            m_matInvViewIsNotActuated = false;
        }
        return m_matInvView;
    };

    void CDevice::MatSet(const hta::CMatrix& mat) {
        m_matViewStack[m_matViewStackTop] = mat;

        m_matInvViewIsNotActuated = true;
        m_matViewIsNotActuated    = true;
        m_updateModelViewProj     = true;
        m_updateModelMatrix       = true;
    };

    void CDevice::MatGetBasis(hta::CVector& right, hta::CVector& up, hta::CVector& fwd) const {
        m_matViewStack[m_matViewStackTop].GetInverseBasis(right, up, fwd);
        right.Normalize();
        up.Normalize();
        fwd.Normalize();
    };

    hta::CVector CDevice::MatGetOrgInv() {
        if (m_matInvViewIsNotActuated) {
            m_matInvView              = ~m_matViewStack[m_matViewStackTop];
            m_matInvViewIsNotActuated = false;
        }

        return m_matInvView.GetOrigin();
    };

    hta::CVector CDevice::MatGetOrg() const {
        return m_matViewStack[m_matViewStackTop].GetOrigin();
    };

    void CDevice::MatGetYPR(float& y, float& p, float& r) {
        throw std::runtime_error("not implemented");
    };

    void CDevice::MatSetWorld(const hta::CMatrix& mat) {
        m_matWorldStack[m_matWorldStackTop] = mat;

        m_matWorldIsNotActuated        = true;
        m_updateModelViewProjWithWorld = true;
        m_updateModelMatrix            = true;
    };

    const hta::CMatrix& CDevice::MatGetWorld() const {
        return m_matWorldStack[m_matWorldStackTop];
    };

    void CDevice::MatDuplicateWorld() {
        if (m_matWorldStackTop < 63) {
            m_matWorldStackTop++;
        }

        m_matWorldStack[m_matWorldStackTop] = m_matWorldStack[m_matWorldStackTop - 1];

        m_matWorldIsNotActuated = true;
    };

    void CDevice::MatPopWorld() {
        if (m_matWorldStackTop > 0) {
            m_matWorldStackTop--;
        }

        m_matWorldIsNotActuated        = true;
        m_updateModelViewProjWithWorld = true;
        m_updateModelMatrix            = true;
    };

    void CDevice::MatSetProj(const hta::CMatrix& mat) {
        m_matProjStack[m_matProjStackTop] = mat;

        ++m_stats.swMatrices;
        m_pd3dDevice->SetTransform(D3DTS_PROJECTION, (const D3DMATRIX*)&m_matProjStack[m_matProjStackTop]);

        m_updateModelViewProj = true;
        ForceRecalcClipPlanes();
    };

    const hta::CMatrix& CDevice::MatGetProj() const {
        return m_matProjStack[m_matProjStackTop];
    };

    void CDevice::MatDuplicateProj() {
        if (m_matProjStackTop < 63) {
            m_matProjStackTop++;
        }

        m_matProjStack[m_matProjStackTop] = m_matProjStack[m_matProjStackTop - 1];

        m_updateModelViewProj = true;
        ForceRecalcClipPlanes();
    };

    void CDevice::MatPopProj() {
        if (m_matProjStackTop > 0) {
            m_matProjStackTop--;
        }

        ++m_stats.swMatrices;
        m_pd3dDevice->SetTransform(D3DTS_PROJECTION, (const D3DMATRIX*)&m_matProjStack[m_matProjStackTop]);

        m_updateModelViewProj = true;
        ForceRecalcClipPlanes();
    };

    void CDevice::ActuateProjectionMatrix() {
        ++m_stats.swMatrices;

        if (m_fastClipEnabled) {
            m_pd3dDevice->SetTransform(D3DTS_PROJECTION, (const D3DMATRIX*)&m_fastClipPlaneProjMatrix);
        } else {
            m_pd3dDevice->SetTransform(D3DTS_PROJECTION, (const D3DMATRIX*)&m_matProjStack[m_matProjStackTop]);
        }
    };

    const hta::CMatrix& CDevice::GetModelMatrix() {
        static hta::CMatrix modelMatrix;

        if (m_updateModelMatrix) {
            const hta::CMatrix& view  = MatGet();
            const hta::CMatrix& world = MatGetWorld();

            hta::CMatrix worldView = world * view;
            modelMatrix            = worldView * m_viewMatrixInv;

            m_updateModelMatrix = false;
        }

        return modelMatrix;
    };

    const hta::CMatrix& CDevice::GetModelViewProjMatrix() {
        static hta::CMatrix modelViewProj;

        if (m_updateModelViewProjWithWorld) {
            const hta::CMatrix& view  = m_matViewStack[m_matViewStackTop];
            const hta::CMatrix& world = m_matWorldStack[m_matWorldStackTop];
            const hta::CMatrix& proj  = m_matProjStack[m_matProjStackTop];

            hta::CMatrix worldView = world * view;
            modelViewProj          = worldView * proj;
        } else if (m_updateModelViewProj) {
            const hta::CMatrix& view = m_matViewStack[m_matViewStackTop];
            const hta::CMatrix& proj = m_matProjStack[m_matProjStackTop];

            modelViewProj = view * proj;
        }

        m_updateModelViewProj          = false;
        m_updateModelViewProjWithWorld = false;

        return modelViewProj;
    };

    hta::CVector CDevice::Unproject(const hta::CVector2& scr) {
        const Viewport& viewport = GetViewport();
        const hta::CMatrix& view = MatGet();
        const hta::CMatrix& proj = MatGetProj();

        float ndcX = ((scr.x * 2.0f) / viewport.m_width) - 1.0f;
        float ndcY = ((scr.y * 2.0f) / viewport.m_height) - 1.0f;

        float viewX = ndcX / proj.m11;
        float viewY = -(ndcY / proj.m22);

        hta::CVector dir = view * hta::CVector(viewX, viewY, 1.0f);

        return dir.Normalized();
    }

    hta::CVector CDevice::Project(const hta::CVector& world) {
        const Viewport& viewport = GetViewport();
        const hta::CMatrix& view = MatGet();
        const hta::CMatrix& proj = MatGetProj();

        hta::CVector viewPos = view * world;

        float ndcX = (viewPos.x * proj.m11) / viewPos.z;
        float ndcY = (viewPos.y * proj.m22) / viewPos.z;

        float screenX = ((ndcX / 2.0f) * viewport.m_width) + (viewport.m_width / 2.0f);
        float screenY = (viewport.m_height / 2.0f) - ((ndcY / 2.0f) * viewport.m_height);

        return hta::CVector(screenX, screenY, viewPos.z);
    }

    hta::CVector CDevice::ProjectWorldAbs(const hta::CVector& world) {
        hta::CVector result;

        D3DXVec3Project(
            (D3DXVECTOR3*)&result,
            (const D3DXVECTOR3*)&world,
            &m_curViewportD3D,
            (const D3DXMATRIX*)&m_matProjStack[m_matProjStackTop],
            (const D3DXMATRIX*)&m_matViewStack[m_matViewStackTop],
            nullptr
        );

        return result;
    };

    void CDevice::TgEnableSetLinearSt(
        int stage, float sx, float sz, float tx, float tz, float roty, bool camSpace, float u0, float v0, float u1, float v1
    ) {
        hta::CMatrix shift;
        shift.Identity();
        shift(3, 0) = -tx; // Was ._41
        shift(3, 2) = -tz; // Was ._43

        hta::CMatrix scale;
        scale.Identity();
        scale(0, 0) = sx; // Was ._11
        scale(2, 2) = sz; // Was ._33

        hta::CMatrix combined = scale * shift;

        float sinRot = sinf(roty);
        float cosRot = cosf(roty);

        hta::CMatrix rotation;
        rotation.Identity();
        rotation(0, 0) = cosRot;  // Was ._11
        rotation(0, 2) = -sinRot; // Was ._13
        rotation(2, 0) = sinRot;  // Was ._31
        rotation(2, 2) = cosRot;  // Was ._33

        combined = rotation * combined;

        hta::CMatrix shiftHalf;
        shiftHalf.Identity();
        shiftHalf(3, 0) = 0.5f; // Was ._41
        shiftHalf(3, 2) = 0.5f; // Was ._43

        combined = shiftHalf * combined;

        hta::CMatrix uvChange;
        uvChange.Identity();

        combined = uvChange * combined;

        TgEnableSetMatrixSt(stage, combined, camSpace);
    }

    void CDevice::TgEnableSetMatrixSt(int stage, const hta::CMatrix& m, bool camSpace) {
        if (m_curTexStagesStates[stage][D3DTSS_TEXTURETRANSFORMFLAGS] != D3DTTFF_COUNT2) {
            ++m_stats.swTextureStageStates;
            m_curTexStagesStates[stage][D3DTSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_COUNT2;
            m_pd3dDevice->SetTextureStageState(stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
        }

        TgSetTcSource(stage, TC_FROM_POSITION_IN_CAMERA_SPACE, stage);

        if (camSpace) {
            SetXFormMatrix(static_cast<D3DTRANSFORMSTATETYPE>(stage + 16), m);
        } else {
            const hta::CMatrix& viewInv = MatGetInv();
            hta::CMatrix worldSpaceMat  = m * viewInv;
            SetXFormMatrix(static_cast<D3DTRANSFORMSTATETYPE>(stage + 16), worldSpaceMat);
        }
    };

    void CDevice::TgEnableSetMatrixStr(int stage, const hta::CMatrix& m, bool camSpace) {
        if (m_curTexStagesStates[stage][D3DTSS_TEXTURETRANSFORMFLAGS] != D3DTTFF_COUNT3) {
            ++m_stats.swTextureStageStates;
            m_curTexStagesStates[stage][D3DTSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_COUNT3;
            m_pd3dDevice->SetTextureStageState(stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
        }

        TgSetTcSource(stage, TC_FROM_POSITION_IN_CAMERA_SPACE, stage);

        if (camSpace) {
            SetXFormMatrix(static_cast<D3DTRANSFORMSTATETYPE>(stage + 16), m);
        } else {
            const hta::CMatrix& viewInv = MatGetInv();
            hta::CMatrix worldSpaceMat  = m * viewInv;
            SetXFormMatrix(static_cast<D3DTRANSFORMSTATETYPE>(stage + 16), worldSpaceMat);
        }
    };

    void CDevice::TgEnableSetMatrixStrReflection(int stage, const hta::CMatrix& m, bool camSpace) {
        // Enable 3D texture transform (D3DTTFF_COUNT3)
        if (m_curTexStagesStates[stage][D3DTSS_TEXTURETRANSFORMFLAGS] != D3DTTFF_COUNT3) {
            ++m_stats.swTextureStageStates;
            m_curTexStagesStates[stage][D3DTSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_COUNT3;
            m_pd3dDevice->SetTextureStageState(stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
        }

        // Set texture coordinate source to reflection vectors in camera space
        TgSetTcSource(stage, TC_FROM_REFLECTION_IN_CAMERA_SPACE, stage);

        if (camSpace) {
            SetXFormMatrix(static_cast<D3DTRANSFORMSTATETYPE>(stage + 16), m);
        } else {
            const hta::CMatrix& viewInv = MatGetInv();
            hta::CMatrix worldSpaceMat  = m * viewInv;
            SetXFormMatrix(static_cast<D3DTRANSFORMSTATETYPE>(stage + 16), worldSpaceMat);
        }
    };

    void CDevice::TgSetTransformMode(int stage, TgMode mode) {
        unsigned int flags = 0;

        switch (mode) {
        case TG_THRU_1:
            flags = D3DTTFF_COUNT1;
            break;
        case TG_THRU_2:
            flags = D3DTTFF_COUNT2;
            break;
        case TG_THRU_3:
            flags = D3DTTFF_COUNT3;
            break;
        case TG_THRU_4:
            flags = D3DTTFF_COUNT4;
            break;
        case TG_PROJ_1:
            flags = D3DTTFF_COUNT1 | D3DTTFF_PROJECTED;
            break;
        case TG_PROJ_2:
            flags = D3DTTFF_COUNT2 | D3DTTFF_PROJECTED;
            break;
        case TG_PROJ_3:
            flags = D3DTTFF_COUNT3 | D3DTTFF_PROJECTED;
            break;
        case TG_PROJ_4:
            flags = D3DTTFF_COUNT4 | D3DTTFF_PROJECTED;
            break;
        case TG_PROJ_PS11:
            flags = D3DTTFF_PROJECTED;
            break;
        default:
            flags = D3DTTFF_DISABLE;
            break;
        }

        if (m_curTexStagesStates[stage][D3DTSS_TEXTURETRANSFORMFLAGS] != flags) {
            ++m_stats.swTextureStageStates;
            m_curTexStagesStates[stage][D3DTSS_TEXTURETRANSFORMFLAGS] = flags;
            m_pd3dDevice->SetTextureStageState(stage, D3DTSS_TEXTURETRANSFORMFLAGS, flags);
        }
    };

    void CDevice::TgSetTcSource(int32_t stage, TcSource mode, int32_t index) {
        switch (mode) {
        case TC_FROM_NORMAL_IN_CAMERA_SPACE:
            index = D3DTSS_TCI_CAMERASPACENORMAL;
            break;
        case TC_FROM_POSITION_IN_CAMERA_SPACE:
            index = D3DTSS_TCI_CAMERASPACEPOSITION;
            break;
        case TC_FROM_REFLECTION_IN_CAMERA_SPACE:
            index = D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR;
            break;
        }

        if (m_curTexStagesStates[stage][D3DTSS_TEXCOORDINDEX] != index) {
            ++m_stats.swTextureStageStates;
            m_curTexStagesStates[stage][D3DTSS_TEXCOORDINDEX] = index;
            m_pd3dDevice->SetTextureStageState(stage, D3DTSS_TEXCOORDINDEX, index);
        }
    }

    void CDevice::TgDisable(int stage) {
        TgSetTransformMode(stage, TG_DISABLE);
        TgSetTcSource(stage, TC_FROM_VERTEX, stage);
    };

    void CDevice::SetTextureMatrix(int stage, const hta::CMatrix& mat) {
        SetXFormMatrix(static_cast<D3DTRANSFORMSTATETYPE>(stage + 16), mat);
    };

    void CDevice::Set2x2BumpMatrix(int stage, float m00, float m01, float m10, float m11) {
        m_pd3dDevice->SetTextureStageState(stage, D3DTSS_BUMPENVMAT00, *reinterpret_cast<DWORD*>(&m00));
        m_pd3dDevice->SetTextureStageState(stage, D3DTSS_BUMPENVMAT01, *reinterpret_cast<DWORD*>(&m01));
        m_pd3dDevice->SetTextureStageState(stage, D3DTSS_BUMPENVMAT10, *reinterpret_cast<DWORD*>(&m10));
        m_pd3dDevice->SetTextureStageState(stage, D3DTSS_BUMPENVMAT11, *reinterpret_cast<DWORD*>(&m11));
    };

    static int tstDepthStack = 0;

    int32_t CDevice::RenderToTexStart(const TexHandle& destTex, bool wantDepth) {
        if (destTex.m_handle < 0)
            return false;

        assert(tstDepthStack == 0 && "Stacking render to tex!");
        tstDepthStack++;

        int width, height;
        GetDims(destTex, width, height);

        Sampler* texture                = this->mActiveTextures.GetItem(destTex.m_handle);
        IDirect3DBaseTexture9* pTexBase = texture->m_maps[0]->mHandleBase;

        m_rtsPtr = nullptr;
        _SetLastResult(static_cast<IDirect3DTexture9*>(pTexBase)->GetSurfaceLevel(0, &m_rtsPtr));
        if (FAILED(m_lastResult)) {
            LOG_ERROR("d3d: ERROR! RenderToTexStart:: Cannot get surface level");
            return false;
        }

        m_rtsSaveColor = nullptr;
        _SetLastResult(m_pd3dDevice->GetRenderTarget(0, &m_rtsSaveColor));
        if (FAILED(m_lastResult)) {
            LOG_ERROR("d3d: ERROR! RenderToTexStart:: Cannot get rt");
            m_rtsPtr->Release();
            return false;
        }

        m_rtsSaveZs = nullptr;
        _SetLastResult(m_pd3dDevice->GetDepthStencilSurface(&m_rtsSaveZs));
        if (FAILED(m_lastResult)) {
            LOG_ERROR("d3d: ERROR! RenderToTexStart:: Cannot get depth stencil");
            m_rtsPtr->Release();
            m_rtsSaveColor->Release();
            return false;
        }

        m_rtsSaveViewport = GetViewport();

        m_rtsNewZs = nullptr;
        if (wantDepth) {
            auto it = m_rtsZSurfaces.find(static_cast<unsigned int>(height));
            if (it == m_rtsZSurfaces.end()) {
                IDirect3DSurface9* newZSurface = nullptr;
                _SetLastResult(m_pd3dDevice->CreateDepthStencilSurface(
                    width, height, m_depthStencilFormatRt, D3DMULTISAMPLE_NONE, 0, TRUE, &newZSurface, nullptr
                ));

                if (FAILED(m_lastResult)) {
                    LOG_ERROR("d3d: ERROR! RenderToTexStart:: Cannot create zsurface");
                }

                it = m_rtsZSurfaces.insert({static_cast<unsigned int>(height), newZSurface}).first;
            }

            m_rtsNewZs = it->second;
            if (m_rtsNewZs)
                m_rtsNewZs->AddRef();
        }

        m_rtsWantZ = wantDepth;

        if (!wantDepth) {
            PushZbState(ZB_DISABLE);
        }

        _SetLastResult(m_pd3dDevice->SetRenderTarget(0, m_rtsPtr));
        if (FAILED(m_lastResult)) {
            LOG_ERROR("d3d: ERROR! RenderToTexStart:: Cannot set rt");
            return false;
        }

        _SetLastResult(m_pd3dDevice->SetDepthStencilSurface(m_rtsNewZs));
        if (FAILED(m_lastResult)) {
            LOG_ERROR("d3d: ERROR! RenderToTexStart:: Cannot set depth stencil surface");
        }

        MatDuplicate();
        MatDuplicateProj();

        Viewport newViewport;
        newViewport.m_x0     = 0;
        newViewport.m_y0     = 0;
        newViewport.m_width  = width;
        newViewport.m_height = height;
        newViewport.m_zMin   = 0.0f;
        newViewport.m_zMax   = 1.0f;
        SetViewport(newViewport);

        m_activeStencilTarget = 1;
        m_stencilLevel[1]     = -1;
        ++m_stats.swRenderTargets;

        return true;
    }

    void CDevice::RenderToTexFinish() {
        if (m_rtsPtr) {
            tstDepthStack--;
            m_rtsPtr->Release();

            _SetLastResult(m_pd3dDevice->SetRenderTarget(0, m_rtsSaveColor));
            _SetLastResult(m_pd3dDevice->SetDepthStencilSurface(m_rtsSaveZs));

            SetViewport(m_rtsSaveViewport);

            if (m_rtsSaveColor)
                m_rtsSaveColor->Release();

            if (m_rtsSaveZs)
                m_rtsSaveZs->Release();

            if (m_rtsNewZs)
                m_rtsNewZs->Release();

            MatPop(false);
            MatPopProj();

            if (!m_rtsWantZ)
                PopZbState();

            m_activeStencilTarget = 0;
        }
    };

    void CDevice::CopyRenderTargetToTexture(const TexHandle& destTex) {
        if (destTex.m_handle < 0)
            return;

        IDirect3DSurface9* srcSurf = nullptr;
        if (FAILED(m_pd3dDevice->GetRenderTarget(0, &srcSurf)))
            return;

        IDirect3DBaseTexture9* pTexBase = this->mActiveTextures.GetItem(destTex.m_handle)->m_maps[0]->mHandleBase;

        IDirect3DSurface9* dstSurf = nullptr;
        if (SUCCEEDED(static_cast<IDirect3DTexture9*>(pTexBase)->GetSurfaceLevel(0, &dstSurf))) {
            m_pd3dDevice->StretchRect(srcSurf, nullptr, dstSurf, nullptr, D3DTEXF_NONE);
            dstSurf->Release();
        }

        srcSurf->Release();
    };

    int CDevice::SetActiveState(int state) {
        m_isActive = state;
        return state;
    }

    int CDevice::CanRender() {
        _SetLastResult(m_pd3dDevice->TestCooperativeLevel());

        if (SUCCEEDED(m_lastResult))
            return 1;

        if (m_lastResult == D3DERR_DEVICELOST)
            return 0;

        if (m_lastResult != D3DERR_DEVICENOTRESET)
            return 0;

        if (Reset())
            return 1;

        hta::m3d::EngineConfig& cfg = hta::m3d::Kernel::Instance()->GetEngineCfg();
        bool fullScreen = (cfg.m_r_fullScreen.m_type == hta::m3d::CVar::CVAR_BOOL) ? cfg.m_r_fullScreen.m_b : (cfg.m_r_fullScreen.m_i > 0);
        int width       = cfg.m_r_width.m_i;
        int height      = cfg.m_r_height.m_i;

        if (SwitchDisplayModes(cfg.m_mainWnd, width, height, fullScreen))
            return 2;

        return 1;
    };

    int32_t CDevice::BeginScene() {
        this->mBindedStreams.fill(nullptr);

        _SetLastResult(m_pd3dDevice->BeginScene());
        m_inScene = SUCCEEDED(m_lastResult);
        return InScene();
    };

    int32_t CDevice::InScene() {
        return m_inScene;
    };

    int32_t CDevice::EndScene() {
        DrawFsRt();
        _SetLastResult(m_pd3dDevice->EndScene());
        m_inScene = FAILED(m_lastResult);
        return !InScene();
    };

    int32_t CDevice::PresentScene() {
        ++m_presents;
        m_viewMatrixWasSetThisFrame = false;
        _SetLastResult(m_pd3dDevice->Present(nullptr, nullptr, nullptr, nullptr));
        return SUCCEEDED(m_lastResult);
    };

    void CDevice::ClearViewport(ClearFlags flags, unsigned int bkClr) {
        IDirect3DSurface9* pDepthStencil = nullptr;
        m_pd3dDevice->GetDepthStencilSurface(&pDepthStencil);

        DWORD clearFlags = (DWORD)flags;

        if (!pDepthStencil) {
            if (m_rtsPtr == nullptr) { // ✅ ADD THIS CHECK
                LOG_WARNING("ZBuffer is empty outside RTT!");
            }
            clearFlags &= ~(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
        }

        if (pDepthStencil)
            pDepthStencil->Release();

        HRESULT hr = m_pd3dDevice->Clear(0, nullptr, clearFlags, bkClr, 1.0f, 0);
        _SetLastResult(hr);

        if (flags & D3DCLEAR_STENCIL)
            m_stencilLevel[m_activeStencilTarget] = 0;
    }

    Viewport CDevice::GetViewport() const {
        return m_curViewport;
    };

    int32_t CDevice::SetViewport(const Viewport& port) {
        m_curViewport    = port;
        m_curViewportD3D = *(D3DVIEWPORT9*)&port;
        return SUCCEEDED(m_pd3dDevice->SetViewport(&m_curViewportD3D));
    };

    void CDevice::RegisterResetCallback(hta::m3d::IDeviceResetCallback* callback) {
        auto it = std::find(m_resetCallbacks.begin(), m_resetCallbacks.end(), callback);
        if (it != m_resetCallbacks.end()) {
            LOG_ERROR("Empty reset callbacks!");
        }
        m_resetCallbacks.push_back(callback);
    };

    void CDevice::UnregisterResetCallback(hta::m3d::IDeviceResetCallback* callback) {
        auto it = std::find(m_resetCallbacks.begin(), m_resetCallbacks.end(), callback);
        if (it == m_resetCallbacks.end()) {
            LOG_ERROR("Reset callback not found!");
            return;
        }

        m_resetCallbacks.erase(it);
    };

    void CDevice::SetGamma(float, float, float) {};

    bool CDevice::IsFeatureSupported(DeviceFeature f) const {
        return m_featureSupported[f];
    };

    void CDevice::SetStageState(int stage, BlendMode mode, TextureState state) {
        D3DTEXTURESTAGESTATETYPE arg1, arg2, op;

        if (mode == BM_COLOR) {
            arg1 = D3DTSS_COLORARG1;
            arg2 = D3DTSS_COLORARG2;
            op   = D3DTSS_COLOROP;
        } else {
            arg1 = D3DTSS_ALPHAARG1;
            arg2 = D3DTSS_ALPHAARG2;
            op   = D3DTSS_ALPHAOP;
        }

        switch (state) {
        case TS_NONE:
            setTextureStageState(stage, op, D3DTOP_DISABLE);
            break;

        case TS_TEXTURE:
            setTextureStageState(stage, arg1, D3DTA_TEXTURE);
            setTextureStageState(stage, op, D3DTOP_SELECTARG1);
            break;

        case TS_DIFFUSE:
            setTextureStageState(stage, arg1, D3DTA_DIFFUSE);
            setTextureStageState(stage, op, D3DTOP_SELECTARG1);
            break;

        case TS_TFACTOR:
            setTextureStageState(stage, arg1, D3DTA_TFACTOR);
            setTextureStageState(stage, op, D3DTOP_SELECTARG1);
            break;

        case TS_MODULATE:
            setTextureStageState(stage, arg1, D3DTA_TEXTURE);
            setTextureStageState(stage, op, D3DTOP_MODULATE);
            setTextureStageState(stage, arg2, D3DTA_DIFFUSE);
            break;

        case TS_MODULATE2X:
            setTextureStageState(stage, arg1, D3DTA_TEXTURE);
            setTextureStageState(stage, op, D3DTOP_MODULATE2X);
            setTextureStageState(stage, arg2, D3DTA_DIFFUSE);
            break;

        case TS_TEX_ADDSIGNED_DIFF:
            setTextureStageState(stage, arg1, D3DTA_TEXTURE);
            setTextureStageState(stage, op, D3DTOP_ADDSIGNED);
            setTextureStageState(stage, arg2, D3DTA_DIFFUSE);
            break;

        case TS_ITEX_ADDSIGNED_DIFF:
            setTextureStageState(stage, arg1, D3DTA_TEXTURE | D3DTA_COMPLEMENT);
            setTextureStageState(stage, op, D3DTOP_ADDSIGNED);
            setTextureStageState(stage, arg2, D3DTA_DIFFUSE);
            break;

        case TS_TEX_DP3_TFAC:
            setTextureStageState(stage, arg1, D3DTA_TEXTURE);
            setTextureStageState(stage, op, D3DTOP_DOTPRODUCT3);
            setTextureStageState(stage, arg2, D3DTA_TFACTOR);
            break;

        case TS_TEX_ADD_DIFF:
            setTextureStageState(stage, arg1, D3DTA_TEXTURE);
            setTextureStageState(stage, op, D3DTOP_ADD);
            setTextureStageState(stage, arg2, D3DTA_DIFFUSE);
            break;

        case TS_PREV:
            setTextureStageState(stage, arg1, D3DTA_CURRENT);
            setTextureStageState(stage, op, D3DTOP_SELECTARG1);
            break;

        case TS_IPREV:
            setTextureStageState(stage, arg1, D3DTA_CURRENT | D3DTA_COMPLEMENT);
            setTextureStageState(stage, op, D3DTOP_SELECTARG1);
            break;

        case TS_IPREV_ADDSIGNED_DIFF:
            setTextureStageState(stage, arg1, D3DTA_DIFFUSE);
            setTextureStageState(stage, op, D3DTOP_ADDSIGNED);
            setTextureStageState(stage, arg2, D3DTA_CURRENT | D3DTA_COMPLEMENT);
            break;

        case TS_IPREV_ADD_DIFF:
            setTextureStageState(stage, arg1, D3DTA_DIFFUSE);
            setTextureStageState(stage, op, D3DTOP_ADD);
            setTextureStageState(stage, arg2, D3DTA_CURRENT | D3DTA_COMPLEMENT);
            break;

        case TS_TEX_ADD_PREV:
            setTextureStageState(stage, arg1, D3DTA_TEXTURE);
            setTextureStageState(stage, op, D3DTOP_ADD);
            setTextureStageState(stage, arg2, D3DTA_CURRENT);
            break;

        case TS_TEX_ADDSMOOTH_PREV:
            setTextureStageState(stage, arg1, D3DTA_TEXTURE);
            setTextureStageState(stage, op, D3DTOP_ADDSMOOTH);
            setTextureStageState(stage, arg2, D3DTA_CURRENT);
            break;

        case TS_TEX_MODULATE_PREV:
            setTextureStageState(stage, arg1, D3DTA_TEXTURE);
            setTextureStageState(stage, op, D3DTOP_MODULATE);
            setTextureStageState(stage, arg2, D3DTA_CURRENT);
            break;

        case TS_TEX_MODULATE2X_PREV:
            setTextureStageState(stage, arg1, D3DTA_TEXTURE);
            setTextureStageState(stage, op, D3DTOP_MODULATE2X);
            setTextureStageState(stage, arg2, D3DTA_CURRENT);
            break;

        case TS_TEX_ADDSIGNED_PREV:
            setTextureStageState(stage, arg1, D3DTA_TEXTURE);
            setTextureStageState(stage, op, D3DTOP_ADDSIGNED);
            setTextureStageState(stage, arg2, D3DTA_CURRENT);
            break;

        case TS_DIFF_MODULATE_PREV:
            setTextureStageState(stage, arg1, D3DTA_DIFFUSE);
            setTextureStageState(stage, op, D3DTOP_MODULATE);
            setTextureStageState(stage, arg2, D3DTA_CURRENT);
            break;

        case TS_DIFF_ADD_PREV:
            setTextureStageState(stage, arg1, D3DTA_DIFFUSE);
            setTextureStageState(stage, op, D3DTOP_ADD);
            setTextureStageState(stage, arg2, D3DTA_CURRENT);
            break;

        case TS_DIFF_ADDSMOOTH_PREV:
            setTextureStageState(stage, arg1, D3DTA_DIFFUSE);
            setTextureStageState(stage, op, D3DTOP_ADDSMOOTH);
            setTextureStageState(stage, arg2, D3DTA_CURRENT);
            break;

        case TS_DIFF_MODULATE_TFAC:
            setTextureStageState(stage, arg1, D3DTA_DIFFUSE);
            setTextureStageState(stage, op, D3DTOP_MODULATE);
            setTextureStageState(stage, arg2, D3DTA_TFACTOR);
            break;

        case TS_TEX_TFAC_LERP_PREV:
            setTextureStageState(stage, op, D3DTOP_LERP);
            setTextureStageState(stage, arg1, D3DTA_CURRENT);
            setTextureStageState(stage, arg2, D3DTA_TEXTURE);
            break;

        case TS_PREV_MINUS_TEX:
            setTextureStageState(stage, arg1, D3DTA_CURRENT);
            setTextureStageState(stage, op, D3DTOP_SUBTRACT);
            setTextureStageState(stage, arg2, D3DTA_TEXTURE);
            break;

        case TS_PREV_ADDSMOOTH_TFAC:
            setTextureStageState(stage, arg1, D3DTA_TFACTOR);
            setTextureStageState(stage, op, D3DTOP_ADDSMOOTH);
            setTextureStageState(stage, arg2, D3DTA_CURRENT);
            break;

        case TS_TEX_MODULATE_TFAC:
            setTextureStageState(stage, arg1, D3DTA_TEXTURE);
            setTextureStageState(stage, op, D3DTOP_MODULATE);
            setTextureStageState(stage, arg2, D3DTA_TFACTOR);
            break;

        case TS_TEX_MODULATE2X_TFAC:
            setTextureStageState(stage, arg1, D3DTA_TEXTURE);
            setTextureStageState(stage, op, D3DTOP_MODULATE2X);
            setTextureStageState(stage, arg2, D3DTA_TFACTOR);
            break;
        }
    }

    void CDevice::SetStreamFrequency(int stage, StreamDataType streamType, unsigned int frequency) {
        unsigned int freq;

        switch (streamType) {
        case M3DSDT_DEFAULT_DATA:
            freq = frequency;
            break;

        case M3DSDT_INDEXED_DATA:
            freq = frequency | D3DSTREAMSOURCE_INDEXEDDATA;
            break;

        case M3DSDT_INSTANCED_DATA:
            freq = frequency | D3DSTREAMSOURCE_INSTANCEDATA;
            break;

        default:
            return;
        }

        if (m_curStreamFreq[stage] != freq) {
            m_curStreamFreq[stage] = freq;
            m_pd3dDevice->SetStreamSourceFreq(stage, freq);
        }
    };

    float CDevice::GetMaxPointSize() {
        return m_d3dCaps.MaxPointSize;
    }

    float CDevice::GetMaxNPatchTessellationLevel() {
        return m_d3dCaps.MaxNpatchTessellationLevel;
    };

    int CDevice::GetMaxVertexShaderConst() {
        return 128;
    };

    const char* getD3dErrorStr(HRESULT hr) {
        switch (hr) {
        case D3D_OK:
            return "D3D_OK";
        case D3DERR_DEVICELOST:
            return "D3DERR_DEVICELOST";
        case D3DERR_INVALIDCALL:
            return "D3DERR_INVALIDCALL";
        case D3DERR_NOTAVAILABLE:
            return "D3DERR_NOTAVAILABLE";
        case D3DERR_OUTOFVIDEOMEMORY:
            return "D3DERR_OUTOFVIDEOMEMORY";
        case D3DERR_DRIVERINTERNALERROR:
            return "D3DERR_DRIVERINTERNALERROR";
        case D3DXERR_INVALIDDATA:
            return "D3DXERR_INVALIDDATA";
        case D3DXERR_INVALIDMESH:
            return "D3DXERR_INVALIDMESH";
        case E_OUTOFMEMORY:
            return "E_OUTOFMEMORY";
        default:
            return "unknown";
        }
    };

    Texture* CDevice::addTexMap(const hta::CStr& filename, unsigned int flags) {
        // Check if texture already exists
        for (auto& [_, image] : this->mActiveImages) {
            if (image->mFileName == filename) {
                image->AddRef();
                return image;
            }
        }

        // Open file stream
        hta::m3d::fs::FileServer& fs     = hta::m3d::Kernel::Instance()->GetFileServer();
        hta::m3d::fs::FileStream* stream = fs.CreateFileStream();
        if (!stream) {
            LOG_ERROR("Failed to create file stream");
            return nullptr;
        }

        if (!stream->Open(filename.c_str(), hta::m3d::fs::IStream::OPEN_READ)) {
            LOG_INFO("Cannot open %s", filename.c_str());
            stream->~FileStream();
            return nullptr;
        }

        // Read file data
        unsigned int fileSize = stream->GetSize();
        std::vector<unsigned char> fileData(fileSize);
        stream->ReadBytes(fileData.data(), fileSize);

        // Determine mip levels and filter
        unsigned int mipLevels = D3DX_DEFAULT;
        DWORD filter           = D3DX_FILTER_NONE;

        if ((flags & 3) == 1) {
            filter = D3DX_FILTER_POINT;
        } else if ((flags & 3) == 2) {
            filter = D3DX_FILTER_LINEAR;
        } else {
            filter    = D3DX_FILTER_NONE;
            mipLevels = 0;
        }

        // Determine format
        D3DFORMAT fmt         = (flags & 4) ? m_texFormat[0][1] : m_texFormat[0][0];
        D3DFORMAT originalFmt = fmt;

        // Get image info
        D3DXIMAGE_INFO imageInfo;
        D3DXGetImageInfoFromFileInMemory(fileData.data(), fileSize, &imageInfo);

        // Determine texture type
        TexType type = TT_2D_FROM_FILE;
        switch (imageInfo.ResourceType) {
        case D3DRTYPE_TEXTURE:
            type = TT_2D_FROM_FILE;
            break;
        case D3DRTYPE_VOLUMETEXTURE:
            type = TT_3D_FROM_FILE;
            break;
        case D3DRTYPE_CUBETEXTURE:
            type = TT_CUBE_FROM_FILE;
            break;
        }

        // Check if format conversion needed for DXT formats
        bool isDXTSource = (imageInfo.Format >= D3DFMT_DXT1 && imageInfo.Format <= D3DFMT_DXT5);
        bool isDXTTarget = (originalFmt >= D3DFMT_DXT1 && originalFmt <= D3DFMT_DXT5);

        if (isDXTSource || !isDXTTarget) {
            fmt = D3DFMT_UNKNOWN;
        }

        // Create texture map structure
        Texture* texMap = new Texture();

        // Load texture based on type
        HRESULT hr;
        if (type == TT_CUBE_FROM_FILE) {
            texMap->mDimension = Texture::eDimension_CUBE;
            hr                 = D3DXCreateCubeTextureFromFileInMemoryEx(
                m_pd3dDevice,
                fileData.data(),
                fileSize,
                D3DX_DEFAULT,
                mipLevels,
                0,
                fmt,
                D3DPOOL_MANAGED,
                filter,
                filter,
                0,
                nullptr,
                nullptr,
                &texMap->mHandleCube
            );
        } else if (type == TT_3D_FROM_FILE) {
            texMap->mDimension = Texture::eDimension_3D;
            hr                 = D3DXCreateVolumeTextureFromFileInMemoryEx(
                m_pd3dDevice,
                fileData.data(),
                fileSize,
                D3DX_DEFAULT,
                D3DX_DEFAULT,
                D3DX_DEFAULT,
                mipLevels,
                0,
                fmt,
                D3DPOOL_MANAGED,
                filter,
                filter,
                0,
                nullptr,
                nullptr,
                &texMap->mHandle3D
            );
        } else {
            texMap->mDimension = Texture::eDimension_2D;
            hr                 = D3DXCreateTextureFromFileInMemoryEx(
                m_pd3dDevice,
                fileData.data(),
                fileSize,
                D3DX_DEFAULT,
                D3DX_DEFAULT,
                mipLevels,
                0,
                fmt,
                D3DPOOL_MANAGED,
                filter,
                filter,
                0,
                nullptr,
                nullptr,
                &texMap->mHandle2D
            );
        }

        if (FAILED(hr)) {
            LOG_ERROR("CreateTextureFromFileInMemoryEx error: %s file: %s", getD3dErrorStr(hr), filename.c_str());

            if (texMap)
                delete texMap;

            stream->~FileStream();
            return nullptr;
        }

        // Fill texture map data
        texMap->mFileName = filename;
        texMap->mPool     = D3DPOOL_MANAGED;
        texMap->mUsage    = 0;
        texMap->mFormat   = fmt;
        texMap->mFlags    = flags;
        texMap->mFileSize = fileSize;
        texMap->mOrigin   = Texture::eOrigin_FILESYSTEM;

        FILETIME fileTime                = stream->GetDate();
        texMap->mFileTime.dwLowDateTime  = fileTime.dwLowDateTime;
        texMap->mFileTime.dwHighDateTime = fileTime.dwHighDateTime;

        if (type == TT_2D_FROM_FILE) {
            texMap->mHandle2D->GetLevelDesc(0, &texMap->mSurface2D);
        } else if (type == TT_3D_FROM_FILE) {
            texMap->mHandle3D->GetLevelDesc(0, &texMap->mSurface3D);
        } else {
            texMap->mHandleCube->GetLevelDesc(0, &texMap->mSurface2D);
        }

        texMap->mType = type;

        this->mActiveImages.PushItem(texMap);

        stream->~FileStream();

        return texMap;
    };

    Texture* CDevice::addTexMap_(uint32_t, const hta::CStr&, uint32_t) {
        throw std::runtime_error("not implemented");
    };

    TexHandle CDevice::AddTexture(const hta::CStr& filename, unsigned int flags) {
        if (!filename.c_str() || strlen(filename.c_str()) == 0) {
            LOG_INFO("invalid filename for tex: %s", filename.c_str());
            return TexHandle{-1};
        }

        TexHandle shaderHandle = readShader(filename, flags);
        if (shaderHandle.m_handle >= 0) {
            return shaderHandle;
        }

        Texture* texMap = addTexMap(filename, flags);
        if (!texMap) {
            LOG_INFO("texmap failed to load: %s", filename.c_str());
            return TexHandle{-1};
        }

        int32_t handle   = -1;
        Sampler* sampler = this->CreateTexture(handle);

        sampler->mRefs = 0;
        sampler->m_maps.push_back(texMap);
        sampler->m_address[0]    = D3DTADDRESS_WRAP;
        sampler->m_address[1]    = D3DTADDRESS_WRAP;
        sampler->m_address[2]    = D3DTADDRESS_WRAP;
        sampler->m_maxAnisotropy = 1;
        sampler->m_magFilter     = D3DTEXF_LINEAR;
        sampler->m_minFilter     = D3DTEXF_LINEAR;
        sampler->m_borderColor   = 0;
        sampler->m_lodBias       = 0.0f;
        sampler->m_lodMax        = 0;
        sampler->m_fps           = 0;
        sampler->mFileName       = filename;

        if ((flags & 3) != 0) {
            sampler->m_mipFilter = D3DTEXF_POINT;
        } else {
            sampler->m_mipFilter = D3DTEXF_NONE;
            sampler->m_lodBias   = -16.0f;
        }

        for (auto& map : sampler->m_maps) {
            map->AddRef();
        }

        ++sampler->mRefs;
        m_isDevMemStatsValid = false;

        return TexHandle{handle};
    };

    const char* getD3dFmtStr(D3DFORMAT fmt) {
        switch (fmt) {
        case D3DFMT_R8G8B8:
            return "R8G8B8";
        case D3DFMT_A8R8G8B8:
            return "A8R8G8B8";
        case D3DFMT_X8R8G8B8:
            return "X8R8G8B8";
        case D3DFMT_R5G6B5:
            return "R5G6B5";
        case D3DFMT_X1R5G5B5:
            return "X1R5G5B5";
        case D3DFMT_A1R5G5B5:
            return "A1R5G5B5";
        case D3DFMT_A4R4G4B4:
            return "A4R4G4B4";
        case D3DFMT_R3G3B2:
            return "R3G3B2";
        case D3DFMT_A8:
            return "A8";
        case D3DFMT_A8R3G3B2:
            return "A8R3G3B2";
        case D3DFMT_X4R4G4B4:
            return "X4R4G4B4";
        case D3DFMT_A8P8:
            return "A8P8";
        case D3DFMT_P8:
            return "P8";
        case D3DFMT_L8:
            return "L8";
        case D3DFMT_A8L8:
            return "A8L8";
        case D3DFMT_A4L4:
            return "A4L4";
        case D3DFMT_V8U8:
            return "V8U8";
        case D3DFMT_L6V5U5:
            return "L6V5U5";
        case D3DFMT_X8L8V8U8:
            return "X8L8V8U8";
        case D3DFMT_Q8W8V8U8:
            return "Q8W8V8U8";
        case D3DFMT_V16U16:
            return "V16U16";
        case D3DFMT_D16_LOCKABLE:
            return "D16_LOCKABLE";
        case D3DFMT_D32:
            return "D32";
        case D3DFMT_D15S1:
            return "D15S1";
        case D3DFMT_D24S8:
            return "D24S8";
        case D3DFMT_D24X8:
            return "D24X8";
        case D3DFMT_D24X4S4:
            return "D24X4S4";
        case D3DFMT_D16:
            return "D16";
        case D3DFMT_VERTEXDATA:
            return "VERTEXDATA";
        case D3DFMT_INDEX16:
            return "INDEX16";
        case D3DFMT_INDEX32:
            return "INDEX32";
        case D3DFMT_DXT1:
            return "DXT1";
        case D3DFMT_DXT2:
            return "DXT2";
        case D3DFMT_DXT3:
            return "DXT3";
        case D3DFMT_DXT4:
            return "DXT4";
        case D3DFMT_DXT5:
            return "DXT5";
        case D3DFMT_YUY2:
            return "YUY2";
        case D3DFMT_UYVY:
            return "UYVY";
        default:
            return "unknown";
        }
    };

    TexHandle CDevice::AddDynamicTexture(const char* texName, int sx, int sy, unsigned int format) {
        // Find first free texture slot
        int32_t idx = -1;

        Sampler* texture = this->CreateTexture(idx);

        // Parse format flags
        bool allMips = (format & 0x8000) != 0;
        if (allMips) {
            format &= ~0x8000;
        }

        // Determine texture format, usage, and pool
        D3DFORMAT texFormat;
        DWORD usage  = 0;
        D3DPOOL pool = D3DPOOL_DEFAULT;

        switch (format) {
        case 0:
            texFormat = m_texFormatRt;
            usage     = D3DUSAGE_RENDERTARGET;
            pool      = D3DPOOL_DEFAULT;
            break;

        case 1:
            texFormat = m_texFormatRt;
            pool      = D3DPOOL_MANAGED;
            break;

        case 2:
        case 3:
        case 4:
        case 5:
            texFormat = m_texFormat[0][1];
            pool      = D3DPOOL_MANAGED;
            break;

        case 6:
            texFormat = m_texFormatShadow;
            usage     = D3DUSAGE_RENDERTARGET;
            pool      = D3DPOOL_DEFAULT;
            break;

        case 7:
            texFormat = m_texFormat[0][0];
            pool      = D3DPOOL_MANAGED;
            break;

        case 8:
            texFormat = m_texFormat[1][0];
            pool      = D3DPOOL_MANAGED;
            break;

        case 9:
            texFormat = m_texFormatDepth;
            usage     = D3DUSAGE_DEPTHSTENCIL;
            pool      = D3DPOOL_DEFAULT;
            break;

        default:
            this->DeleteTexture(idx);
            return TexHandle{-1};
        }

        // Create D3D texture
        IDirect3DTexture9* d3dTex = nullptr;
        HRESULT hr                = D3DXCreateTexture(m_pd3dDevice, sx, sy, allMips ? 0 : 1, usage, texFormat, pool, &d3dTex);

        if (FAILED(hr)) {
            const char* usageStr = (usage == D3DUSAGE_RENDERTARGET) ? "rt" : "generic";
            const char* poolStr  = (pool == D3DPOOL_MANAGED) ? "managed" : "default";

            LOG_INFO(
                "failed to create dynamic texture:sx = %d sy = %d usage = %s fmt = %s pool = %s error = %s",
                sx,
                sy,
                usageStr,
                getD3dFmtStr(texFormat),
                poolStr,
                getD3dErrorStr(hr)
            );

            this->DeleteTexture(idx);
            return TexHandle{-1};
        }

        // Create CTexMap wrapper
        Texture* texMap = new Texture();
        this->mActiveImages.PushItem(texMap);

        texMap->mOrigin    = Texture::eOrigin_RUNTIME;
        texMap->mDimension = Texture::eDimension_2D;

        texMap->mHandle2D = d3dTex;
        d3dTex->GetLevelDesc(0, &texMap->mSurface2D);
        texMap->mPool     = pool;
        texMap->mUsage    = usage;
        texMap->mType     = TT_2D_DYNAMIC;
        texMap->mFileName = texName;
        texMap->mFlags    = (allMips ? 1 : 0) | 4;

        // Setup Sampler
        texture->mRefs = 0;
        texture->m_maps.push_back(texMap);
        texture->m_magFilter     = D3DTEXF_LINEAR;
        texture->m_minFilter     = D3DTEXF_LINEAR;
        texture->m_address[0]    = D3DTADDRESS_WRAP;
        texture->m_address[1]    = D3DTADDRESS_WRAP;
        texture->m_address[2]    = D3DTADDRESS_WRAP;
        texture->m_maxAnisotropy = 1;
        texture->m_mipFilter     = allMips ? D3DTEXF_LINEAR : D3DTEXF_NONE;
        texture->m_borderColor   = 0;
        texture->m_lodBias       = 0.0f;
        texture->m_lodMax        = 0;
        texture->m_fps           = 0;
        texture->mFileName       = texName;

        for (auto& map : texture->m_maps) {
            map->AddRef();
        }

        ++texture->mRefs;
        m_isDevMemStatsValid = false;

        return TexHandle{idx};
    };

    TexHandle CDevice::GetFullFrameFrameBufferTexture() {
        if (m_texFrameBufer.m_handle < 0) {
            hta::m3d::EngineConfig& cfg = hta::m3d::Kernel::Instance()->GetEngineCfg();
            m_texFrameBufer             = AddDynamicTexture("$TexFrameBufer", cfg.m_r_width.m_i, cfg.m_r_height.m_i, 0);
            SetTextureParameter(m_texFrameBufer, TM_WRAP_S, D3DTADDRESS_CLAMP);
            SetTextureParameter(m_texFrameBufer, TM_WRAP_T, D3DTADDRESS_CLAMP);
        }

        return m_texFrameBufer;
    };

    bool CDevice::ReportTexturesInfo(const char*) {
        // Categorize textures by type
        std::vector<Texture*> texturesByType[TT_NUM_TYPES];
        unsigned int numTexsByType[TT_NUM_TYPES] = {0};

        for (auto& [_, texMap] : this->mActiveImages) {
            if (texMap) {
                TexType type = texMap->mType;
                numTexsByType[type]++;
                texturesByType[type].push_back(texMap);
            }
        }

        // Report general stats
        LOG_INFO("** Texture Stats **");
        LOG_INFO("2D textures: %u", numTexsByType[TT_2D_FROM_FILE]);
        LOG_INFO("Dynamic 2D textures: %u", numTexsByType[TT_2D_DYNAMIC]);
        LOG_INFO("2D render targets: %u", numTexsByType[TT_2D_RENDER_TARGET]);
        LOG_INFO("Cube maps: %u", numTexsByType[TT_CUBE_FROM_FILE]);
        LOG_INFO("Dynamic cube maps: %u", numTexsByType[TT_CUBE_DYNAMIC]);
        LOG_INFO("Cube map render targets: %u", numTexsByType[TT_CUBE_RENDER_TARGET]);
        LOG_INFO("Volume textures: %u", numTexsByType[TT_3D_FROM_FILE]);
        LOG_INFO("Dynamic volume textures: %u", numTexsByType[TT_3D_DYNAMIC]);

        // Calculate total memory usage
        unsigned int totalSize = 0;
        for (auto& [_, texMap] : this->mActiveImages) {
            if (texMap) {
                // Approximate size calculation
                unsigned int size = texMap->mSurface2D.Width * texMap->mSurface2D.Height;
                if (texMap->mFormat >= D3DFMT_DXT1 && texMap->mFormat <= D3DFMT_DXT5)
                    size /= 2; // DXT compression
                totalSize += size;
            }
        }

        LOG_INFO("Total approx memory: %.2f MB", totalSize / (1024.0f * 1024.0f));
        return true;
    };

    TexHandle CDevice::AddRenderTargetTexture(const char* texName, int sx, int sy) {
        return AddDynamicTexture(texName, sx, sy, 0);
    };

    TexHandle CDevice::GetBufferedTargetTexture(int sz) {
        auto it = m_texBufferedRT.find(sz);
        if (it != m_texBufferedRT.end() && it->second.m_handle >= 0) {
            return it->second;
        }

        TexHandle texHandle = AddDynamicTexture("$TexBufferedRT", sz, sz, 0);
        SetTextureParameter(texHandle, TM_WRAP_S, D3DTADDRESS_CLAMP);
        SetTextureParameter(texHandle, TM_WRAP_T, D3DTADDRESS_CLAMP);
        ReferenceTexture(texHandle);

        m_texBufferedRT[sz] = texHandle;

        return texHandle;
    };

    bool CDevice::IsDynamic(const Sampler& tex) const {
        if (tex.m_maps.empty() || tex.m_maps.size() != 1)
            return false;

        TexType type = tex.m_maps[0]->mType;

        return type == TT_2D_DYNAMIC || type == TT_2D_RENDER_TARGET || type == TT_CUBE_DYNAMIC || type == TT_CUBE_RENDER_TARGET ||
               type == TT_3D_DYNAMIC;
    };

    int CDevice::ReloadTextures() {
        int reloadCount = 0;

        for (auto [slot, texMap] : this->mActiveImages) {
            if (!texMap)
                continue;

            // Skip dynamic textures
            TexType type = texMap->mType;
            if (type == TT_2D_DYNAMIC || type == TT_2D_RENDER_TARGET || type == TT_CUBE_DYNAMIC || type == TT_CUBE_RENDER_TARGET ||
                type == TT_3D_DYNAMIC) {
                continue;
            }

            // Open file
            hta::m3d::fs::FileServer& fs     = hta::m3d::Kernel::Instance()->GetFileServer();
            hta::m3d::fs::FileStream* stream = fs.CreateFileStream();
            if (!stream)
                continue;

            if (!stream->Open(texMap->mFileName.c_str(), hta::m3d::fs::IStream::OPEN_READ)) {
                LOG_INFO("error reloading texture: cannot open %s", texMap->mFileName.c_str());
                delete stream;
                continue;
            }

            // Check if file changed
            unsigned int fileSize = stream->GetSize();
            FILETIME fileTime     = stream->GetDate();

            if (!m_reloadAllTextures && fileSize == texMap->mFileSize && fileTime.dwHighDateTime == texMap->mFileTime.dwHighDateTime &&
                fileTime.dwLowDateTime == texMap->mFileTime.dwLowDateTime) {
                delete stream;
                continue;
            }

            // Read file data
            std::vector<unsigned char> fileData(fileSize);
            stream->ReadBytes(fileData.data(), fileSize);

            // Determine mip levels and filter
            unsigned int mipLevels = D3DX_DEFAULT;
            DWORD filter           = D3DX_FILTER_NONE;
            int flags              = texMap->mFlags & 3;

            if (flags == 1) {
                filter = D3DX_FILTER_POINT;
            } else if (flags == 2) {
                filter = D3DX_FILTER_LINEAR;
            } else {
                filter    = D3DX_FILTER_NONE;
                mipLevels = 0;
            }

            // Get image info
            D3DXIMAGE_INFO imageInfo;
            D3DXGetImageInfoFromFileInMemory(fileData.data(), fileSize, &imageInfo);

            // Determine texture type
            TexType newType = TT_2D_FROM_FILE;
            switch (imageInfo.ResourceType) {
            case D3DRTYPE_TEXTURE:
                newType = TT_2D_FROM_FILE;
                break;
            case D3DRTYPE_VOLUMETEXTURE:
                newType = TT_3D_FROM_FILE;
                break;
            case D3DRTYPE_CUBETEXTURE:
                newType = TT_CUBE_FROM_FILE;
                break;
            }

            // Verify type matches
            if (newType != texMap->mType) {
                LOG_INFO("error reloading texture: types do not match for %s", texMap->mFileName.c_str());
                delete stream;
                continue;
            }

            // Create new texture map
            Texture* newTexMap = new Texture();

            // Load texture based on type
            HRESULT hr;
            if (newType == TT_2D_FROM_FILE) {
                hr = D3DXCreateTextureFromFileInMemoryEx(
                    m_pd3dDevice,
                    fileData.data(),
                    fileSize,
                    D3DX_DEFAULT,
                    D3DX_DEFAULT,
                    mipLevels,
                    0,
                    texMap->mFormat,
                    texMap->mPool,
                    filter,
                    filter,
                    0,
                    nullptr,
                    nullptr,
                    &newTexMap->mHandle2D
                );
            } else if (newType == TT_CUBE_FROM_FILE) {
                hr = D3DXCreateCubeTextureFromFileInMemoryEx(
                    m_pd3dDevice,
                    fileData.data(),
                    fileSize,
                    D3DX_DEFAULT,
                    mipLevels,
                    0,
                    texMap->mFormat,
                    texMap->mPool,
                    filter,
                    filter,
                    0,
                    nullptr,
                    nullptr,
                    &newTexMap->mHandleCube
                );
            } else {
                hr = D3DXCreateVolumeTextureFromFileInMemoryEx(
                    m_pd3dDevice,
                    fileData.data(),
                    fileSize,
                    D3DX_DEFAULT,
                    D3DX_DEFAULT,
                    D3DX_DEFAULT,
                    mipLevels,
                    0,
                    texMap->mFormat,
                    texMap->mPool,
                    filter,
                    filter,
                    0,
                    nullptr,
                    nullptr,
                    &newTexMap->mHandle3D
                );
            }

            if (FAILED(hr)) {
                LOG_ERROR(
                    "error reloading texture: CreateTextureFromFileInMemoryEx error: %s file: %s",
                    getD3dErrorStr(hr),
                    texMap->mFileName.c_str()
                );
                delete newTexMap;
                delete stream;
                continue;
            }

            // Fill new texture map data
            newTexMap->mOrigin    = texMap->mOrigin;
            newTexMap->mDimension = texMap->mDimension;

            newTexMap->mFileName = texMap->mFileName;
            newTexMap->mPool     = D3DPOOL_MANAGED;
            newTexMap->mUsage    = 0;
            newTexMap->mFlags    = texMap->mFlags;
            newTexMap->mFileSize = fileSize;
            newTexMap->mFileTime = fileTime;

            // Get level descriptors
            if (newType == TT_2D_FROM_FILE) {
                newTexMap->mHandle2D->GetLevelDesc(0, &newTexMap->mSurface2D);
            } else if (newType == TT_CUBE_FROM_FILE) {
                newTexMap->mHandleCube->GetLevelDesc(0, &newTexMap->mSurface2D);
            } else if (newType == TT_3D_FROM_FILE) {
                newTexMap->mHandle3D->GetLevelDesc(0, &newTexMap->mSurface3D);
            }

            newTexMap->mType = newType;
            newTexMap->mRefs = texMap->mRefs;

            // Update all references in mActiveTextures
            for (auto& [_, texture] : this->mActiveTextures) {
                for (auto& map : texture->m_maps) {
                    if (map == texMap)
                        map = newTexMap;
                }
            }

            // Replace in mActiveImages
            mActiveImages.DelItem(slot);
            mActiveImages.AddItem(slot, newTexMap);

            // Free old texture and delete
            if (texMap->mHandleBase) {
                texMap->mHandleBase->Release();
                texMap->mHandleBase = nullptr;
            }
            delete texMap;

            ++reloadCount;
            delete stream;
        }

        m_reloadAllTextures = false;
        return reloadCount;
    }

    int32_t CDevice::SetTexture(int stage, const TexHandle& id, double tsc) {
        if (id.m_handle < 0) {
            SetWhiteTexture(stage);
            return true;
        }

        if (!IsTexValid(id))
            return false;

        Sampler* sampler = this->mActiveTextures.GetItem(id.m_handle);
        int frame        = GetTextureCurrentFrame(sampler, tsc);

        assert(sampler && "Tries to set freed texture!");
        setTexture(stage, sampler->m_maps[frame]);

        // Get LOD bias from config
        hta::m3d::EngineConfig& cfg = hta::m3d::Kernel::Instance()->GetEngineCfg();
        float lodBias =
            (cfg.m_r_texLodBias.m_type == hta::m3d::CVar::CVAR_FLOAT) ? cfg.m_r_texLodBias.m_f : static_cast<float>(cfg.m_r_texLodBias.m_i);

        // Set sampler states
        setTextureSamplerState(stage, D3DSAMP_ADDRESSU, sampler->m_address[0]);
        setTextureSamplerState(stage, D3DSAMP_ADDRESSV, sampler->m_address[1]);
        setTextureSamplerState(stage, D3DSAMP_ADDRESSW, sampler->m_address[2]);
        setTextureSamplerState(stage, D3DSAMP_MAXANISOTROPY, sampler->m_maxAnisotropy);
        setTextureSamplerState(stage, D3DSAMP_MINFILTER, sampler->m_minFilter);
        setTextureSamplerState(stage, D3DSAMP_MAGFILTER, sampler->m_magFilter);
        setTextureSamplerState(stage, D3DSAMP_MIPFILTER, sampler->m_mipFilter);
        setTextureSamplerState(stage, D3DSAMP_BORDERCOLOR, sampler->m_borderColor);

        // Use texture's LOD bias if set, otherwise use global config value
        float texLodBias = (sampler->m_lodBias == 0.0f) ? lodBias : sampler->m_lodBias;
        setTextureSamplerState(stage, D3DSAMP_MIPMAPLODBIAS, *reinterpret_cast<DWORD*>(&texLodBias));

        setTextureSamplerState(stage, D3DSAMP_MAXMIPLEVEL, sampler->m_lodMax);

        return true;
    };

    void CDevice::SetWhiteTexture(int stage) {
        m_pd3dDevice->SetTexture(stage, nullptr);
    };

    void CDevice::SetBlackTexture(int stage) {
        m_pd3dDevice->SetTexture(stage, nullptr);
    };

    void CDevice::SetErrorTexture(int stage) {
        m_pd3dDevice->SetTexture(stage, nullptr);
    };

    void CDevice::DisableTextureStages(int stageToStartFrom) {
        if (m_curTexStagesStates[stageToStartFrom][D3DTSS_COLOROP] != D3DTOP_DISABLE) {
            ++m_stats.swTextureStageStates;
            m_curTexStagesStates[stageToStartFrom][D3DTSS_COLOROP] = D3DTOP_DISABLE;
            m_pd3dDevice->SetTextureStageState(stageToStartFrom, D3DTSS_COLOROP, D3DTOP_DISABLE);
        }

        if (m_curTexStagesStates[stageToStartFrom][D3DTSS_ALPHAOP] != D3DTOP_DISABLE) {
            ++m_stats.swTextureStageStates;
            m_curTexStagesStates[stageToStartFrom][D3DTSS_ALPHAOP] = D3DTOP_DISABLE;
            m_pd3dDevice->SetTextureStageState(stageToStartFrom, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        }
    };

    void CDevice::SetTexAnimStart(const TexHandle& texId) {
        if (texId.m_handle >= 0)
            this->mActiveTextures.GetItem(texId.m_handle)->m_timeStamp = 0.0;
    };

    int CDevice::ReferenceTexture(const TexHandle& id) {
        if (!IsTexValid(id))
            return 0;

        Sampler* texture = this->mActiveTextures.GetItem(id.m_handle);

        for (auto& map : texture->m_maps) {
            map->AddRef();
        }

        return ++texture->mRefs;
    };

    int CDevice::ReleaseTexture(TexHandle& id) {
        if (!IsTexValid(id))
            return 0;

        Sampler* texture = this->mActiveTextures.GetItem(id.m_handle);

        if (--texture->mRefs == 0) {
            // Release all maps
            for (auto& map : texture->m_maps) {
                if (map->GetRef() == 1) {
                    for (auto& [slot, img] : mActiveImages) {
                        if (img == map) {
                            mActiveImages.DelItem(slot);
                            break;
                        }
                    }
                    map->DecRef();
                }
            }

            texture->m_maps.clear();
            this->mActiveTextures.DelItem(id.m_handle);
            delete texture;

            id.SetInvalid();
            m_isDevMemStatsValid = false;
            return 0;
        }

        return texture->mRefs;
    }

    int32_t CDevice::GetTextureName(const TexHandle& id, hta::CStr& name) {
        if (IsTexValid(id)) {
            name = this->mActiveTextures.GetItem(id.m_handle)->mFileName;
            return true;
        } else {
            name = "";
            return false;
        }
    };

    void CDevice::SetTextureParameter(const TexHandle& id, TexParam param, uint32_t value) {
        if (!IsTexValid(id))
            return;

        Sampler* texture = this->mActiveTextures.GetItem(id.m_handle);

        switch (param) {
        case TM_TEX_FILTER:
            switch (value) {
            case 2:
                texture->m_magFilter = D3DTEXF_LINEAR;
                texture->m_minFilter = D3DTEXF_LINEAR;
                texture->m_mipFilter = D3DTEXF_NONE;
                break;
            case 3:
                texture->m_magFilter = D3DTEXF_ANISOTROPIC;
                texture->m_minFilter = D3DTEXF_ANISOTROPIC;
                texture->m_mipFilter = D3DTEXF_LINEAR;
                break;
            case 4:
                texture->m_magFilter = D3DTEXF_LINEAR;
                texture->m_minFilter = D3DTEXF_LINEAR;
                texture->m_mipFilter = D3DTEXF_POINT;
                break;
            case 5:
                texture->m_magFilter = D3DTEXF_LINEAR;
                texture->m_minFilter = D3DTEXF_LINEAR;
                texture->m_mipFilter = D3DTEXF_LINEAR;
                break;
            default:
                texture->m_magFilter = D3DTEXF_POINT;
                texture->m_minFilter = D3DTEXF_POINT;
                texture->m_mipFilter = D3DTEXF_NONE;
                break;
            }
            break;

        case TM_TEX_MIN_FILTER:
            switch (value) {
            case 6:
                texture->m_minFilter = D3DTEXF_POINT;
                texture->m_mipFilter = D3DTEXF_POINT;
                break;
            case 7:
                texture->m_minFilter = D3DTEXF_LINEAR;
                texture->m_mipFilter = D3DTEXF_POINT;
                break;
            case 8:
                texture->m_minFilter = D3DTEXF_POINT;
                texture->m_mipFilter = D3DTEXF_LINEAR;
                break;
            case 9:
                texture->m_minFilter = D3DTEXF_LINEAR;
                texture->m_mipFilter = D3DTEXF_LINEAR;
                break;
            case 10:
                texture->m_minFilter = D3DTEXF_ANISOTROPIC;
                texture->m_mipFilter = D3DTEXF_POINT;
                break;
            case 11:
                texture->m_minFilter = D3DTEXF_ANISOTROPIC;
                texture->m_mipFilter = D3DTEXF_LINEAR;
                break;
            default:
                texture->m_minFilter = D3DTEXF_POINT;
                texture->m_mipFilter = D3DTEXF_NONE;
                break;
            }
            break;

        case TM_TEX_MAG_FILTER:
            if (value == 0 || value > 3)
                value = 1;
            texture->m_magFilter = static_cast<D3DTEXTUREFILTERTYPE>(value);
            break;

        case TM_WRAP_S:
            if (value != 1 && (value <= 2 || value > 4))
                value = 1;
            texture->m_address[0] = static_cast<D3DTEXTUREADDRESS>(value);
            break;

        case TM_WRAP_T:
            if (value != 1 && (value <= 2 || value > 4))
                value = 1;
            texture->m_address[1] = static_cast<D3DTEXTUREADDRESS>(value);
            break;

        case TM_WRAP_R:
            if (value != 1 && (value <= 2 || value > 4))
                value = 1;
            texture->m_address[2] = static_cast<D3DTEXTUREADDRESS>(value);
            break;

        case TM_WRAP_BORDER_COLOR:
            texture->m_borderColor = value;
            break;

        case TM_MIP_LOD_BIAS:
            texture->m_lodBias = *reinterpret_cast<float*>(&value);
            break;

        case TM_MIP_LOD_MAX:
            texture->m_lodMax = *reinterpret_cast<int*>(&value);
            break;

        case TM_MAX_ANISOTROPY:
            texture->m_maxAnisotropy = *reinterpret_cast<int*>(&value);
            break;
        }
    };

    int32_t
    CDevice::UploadTexImage(const TexHandle& id, uint32_t sx, uint32_t sy, unsigned char* bits, TexDynFormat incomingFormat, int mipLevel) {
        int pitch               = 0;
        unsigned char* destBits = static_cast<unsigned char*>(LockTexture(id, incomingFormat, pitch, mipLevel));
        if (!destBits)
            return false;

        Texture* texMap = this->mActiveTextures.GetItem(id.m_handle)->m_maps[0];
        uint32_t width  = (texMap->mSurface2D.Width < sx) ? texMap->mSurface2D.Width : sx;
        uint32_t height = (texMap->mSurface2D.Height < sy) ? texMap->mSurface2D.Height : sy;

        bool success = false;

        switch (texMap->mSurface2D.Format) {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
            switch (incomingFormat) {
            case TM_DTF_LUMINANCE8: {
                unsigned char* src = bits;
                uint32_t* dest     = reinterpret_cast<uint32_t*>(destBits);
                int destStride     = pitch / 4;

                for (uint32_t y = 0; y < height; ++y) {
                    for (uint32_t x = 0; x < width; ++x) {
                        unsigned char lum = src[x];
                        dest[x]           = 0xFF000000 | (lum << 16) | (lum << 8) | lum;
                    }
                    src += width;
                    dest += destStride;
                }
                success = true;
                break;
            }

            case TM_DTF_ALPHA8: {
                unsigned char* src = bits;
                uint32_t* dest     = reinterpret_cast<uint32_t*>(destBits);
                int destStride     = pitch / 4;

                for (uint32_t y = 0; y < height; ++y) {
                    for (uint32_t x = 0; x < width; ++x) {
                        dest[x] = src[x] << 24;
                    }
                    src += width;
                    dest += destStride;
                }
                success = true;
                break;
            }

            case TM_DTF_RGBA8888: {
                unsigned char* src = bits;
                uint32_t* dest     = reinterpret_cast<uint32_t*>(destBits);
                int destStride     = pitch / 4;

                for (uint32_t y = 0; y < height; ++y) {
                    memcpy(dest, src, width * 4);
                    src += width * 4;
                    dest += destStride;
                }
                success = true;
                break;
            }

            case TM_DTF_RGBA8888_VIDEOFRAME: {
                memcpy(destBits, bits, width * height * 4);
                success = true;
                break;
            }
            }
            break;

        case D3DFMT_R5G6B5:
            switch (incomingFormat) {
            case TM_DTF_LUMINANCE8: {
                unsigned char* src = bits;
                uint16_t* dest     = reinterpret_cast<uint16_t*>(destBits);
                int destStride     = pitch / 2;

                for (uint32_t y = 0; y < height; ++y) {
                    for (uint32_t x = 0; x < width; ++x) {
                        unsigned char lum = src[x] >> 3;
                        dest[x]           = lum | (lum << 6) | (lum << 11);
                    }
                    src += width;
                    dest += destStride;
                }
                success = true;
                break;
            }

            case TM_DTF_ALPHA8: {
                uint16_t* dest = reinterpret_cast<uint16_t*>(destBits);
                int destStride = pitch / 2;

                for (uint32_t y = 0; y < height; ++y) {
                    memset(dest, 0, width * 2);
                    dest += destStride;
                }
                success = true;
                break;
            }

            case TM_DTF_RGBA8888: {
                uint32_t* src  = reinterpret_cast<uint32_t*>(bits);
                uint16_t* dest = reinterpret_cast<uint16_t*>(destBits);
                int destStride = pitch / 2;

                for (uint32_t y = 0; y < height; ++y) {
                    for (uint32_t x = 0; x < width; ++x) {
                        uint32_t rgba = src[x];
                        dest[x]       = ((rgba >> 3) & 0x1F) | (((rgba >> 10) & 0x3F) << 5) | (((rgba >> 19) & 0x1F) << 11);
                    }
                    src += width;
                    dest += destStride;
                }
                success = true;
                break;
            }
            }
            break;

        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5:
            switch (incomingFormat) {
            case TM_DTF_LUMINANCE8: {
                unsigned char* src = bits;
                uint16_t* dest     = reinterpret_cast<uint16_t*>(destBits);
                int destStride     = pitch / 2;

                for (uint32_t y = 0; y < height; ++y) {
                    for (uint32_t x = 0; x < width; ++x) {
                        unsigned char lum = src[x] >> 3;
                        dest[x]           = lum | (lum << 5) | (lum << 10) | 0x8000;
                    }
                    src += width;
                    dest += destStride;
                }
                success = true;
                break;
            }

            case TM_DTF_ALPHA8: {
                unsigned char* src = bits;
                uint16_t* dest     = reinterpret_cast<uint16_t*>(destBits);
                int destStride     = pitch / 2;

                for (uint32_t y = 0; y < height; ++y) {
                    for (uint32_t x = 0; x < width; ++x) {
                        dest[x] = (src[x] >> 7) << 15;
                    }
                    src += width;
                    dest += destStride;
                }
                success = true;
                break;
            }

            case TM_DTF_RGBA8888: {
                uint32_t* src  = reinterpret_cast<uint32_t*>(bits);
                uint16_t* dest = reinterpret_cast<uint16_t*>(destBits);
                int destStride = pitch / 2;

                for (uint32_t y = 0; y < height; ++y) {
                    for (uint32_t x = 0; x < width; ++x) {
                        uint32_t rgba = src[x];
                        dest[x]       = ((rgba >> 3) & 0x1F) | (((rgba >> 11) & 0x1F) << 5) | (((rgba >> 19) & 0x1F) << 10) |
                                  (((rgba >> 31) & 0x1) << 15);
                    }
                    src += width;
                    dest += destStride;
                }
                success = true;
                break;
            }
            }
            break;

        case D3DFMT_A4R4G4B4:
        case D3DFMT_X4R4G4B4:
            switch (incomingFormat) {
            case TM_DTF_LUMINANCE8: {
                unsigned char* src = bits;
                uint16_t* dest     = reinterpret_cast<uint16_t*>(destBits);
                int destStride     = pitch / 2;

                for (uint32_t y = 0; y < height; ++y) {
                    for (uint32_t x = 0; x < width; ++x) {
                        unsigned char lum = src[x] >> 4;
                        dest[x]           = lum | (lum << 4) | (lum << 8) | 0xF000;
                    }
                    src += width;
                    dest += destStride;
                }
                success = true;
                break;
            }

            case TM_DTF_ALPHA8: {
                unsigned char* src = bits;
                uint16_t* dest     = reinterpret_cast<uint16_t*>(destBits);
                int destStride     = pitch / 2;

                for (uint32_t y = 0; y < height; ++y) {
                    for (uint32_t x = 0; x < width; ++x) {
                        dest[x] = ((src[x] >> 4) << 12) | 0xFFF;
                    }
                    src += width;
                    dest += destStride;
                }
                success = true;
                break;
            }

            case TM_DTF_RGBA8888: {
                uint32_t* src  = reinterpret_cast<uint32_t*>(bits);
                uint16_t* dest = reinterpret_cast<uint16_t*>(destBits);
                int destStride = pitch / 2;

                for (uint32_t y = 0; y < height; ++y) {
                    for (uint32_t x = 0; x < width; ++x) {
                        uint32_t rgba = src[x];
                        dest[x] =
                            ((rgba >> 4) & 0xF) | (((rgba >> 12) & 0xF) << 4) | (((rgba >> 20) & 0xF) << 8) | (((rgba >> 28) & 0xF) << 12);
                    }
                    src += width;
                    dest += destStride;
                }
                success = true;
                break;
            }
            }
            break;
        }

        UnlockTexture(id);
        return success;
    };

    void* CDevice::LockTexture(const TexHandle& id, TexDynFormat incomingFormat, int& pitch, int mipLevel) {
        if (!IsTexValid(id))
            return nullptr;

        IDirect3DBaseTexture9* pTex = this->mActiveTextures.GetItem(id.m_handle)->m_maps[0]->mHandleBase;

        D3DLOCKED_RECT lr;
        HRESULT hr = static_cast<IDirect3DTexture9*>(pTex)->LockRect(mipLevel, &lr, nullptr, D3DLOCK_DISCARD);

        if (FAILED(hr)) {
            LOG_INFO("cannot lock texture surface");
            return nullptr;
        }

        pitch = lr.Pitch;
        return lr.pBits;
    };

    void CDevice::UnlockTexture(const TexHandle& id) {
        IDirect3DBaseTexture9* pTex = this->mActiveTextures.GetItem(id.m_handle)->m_maps[0]->mHandleBase;
        static_cast<IDirect3DTexture9*>(pTex)->UnlockRect(0);
    };

    int32_t CDevice::DownloadPureTexImageRgba8888(void* dstBits, const TexHandle& tex) {
        int width, height;
        GetDims(tex, width, height);
        return DownloadSizedTexImageRgba8888(dstBits, tex, width, height);
    };

    int32_t CDevice::DownloadSizedTexImageRgba8888(void* data, const TexHandle& tex, int sxx, int syy) {
        uint32_t* dstBits = (uint32_t*)data;

        if (!IsTexValid(tex))
            return false;

        Sampler* texture            = this->mActiveTextures.GetItem(tex.m_handle);
        IDirect3DBaseTexture9* pTex = texture->m_maps[0]->mHandleBase;

        // Get source surface
        IDirect3DSurface9* surf = nullptr;
        HRESULT hr              = static_cast<IDirect3DTexture9*>(pTex)->GetSurfaceLevel(0, &surf);
        if (FAILED(hr)) {
            LOG_INFO("cannot get surface for tex");
            return false;
        }

        int texWidth, texHeight;
        GetDims(tex, texWidth, texHeight);

        // Create temporary texture in system memory
        IDirect3DTexture9* destTex = nullptr;
        hr                         = D3DXCreateTexture(m_pd3dDevice, sxx, syy, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &destTex);
        if (FAILED(hr)) {
            LOG_INFO("Cannot create temp texture");
            surf->Release();
            return false;
        }

        // Get destination surface
        IDirect3DSurface9* dest = nullptr;
        hr                      = destTex->GetSurfaceLevel(0, &dest);
        if (FAILED(hr)) {
            LOG_INFO("Cannot get surface2");
            destTex->Release();
            surf->Release();
            return false;
        }

        // Copy surface data
        RECT dstRect = {0, 0, sxx, syy};
        hr           = D3DXLoadSurfaceFromSurface(dest, nullptr, &dstRect, surf, nullptr, nullptr, D3DX_FILTER_NONE, 0);
        surf->Release();

        if (FAILED(hr)) {
            LOG_INFO("Cannot get out texture");
            dest->Release();
            destTex->Release();
            return false;
        }

        // Lock destination surface
        D3DLOCKED_RECT lr;
        hr = dest->LockRect(&lr, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr)) {
            LOG_INFO("cannot lock surf");
            dest->Release();
            destTex->Release();
            return false;
        }

        // Get surface description
        D3DSURFACE_DESC desc;
        destTex->GetLevelDesc(0, &desc);

        bool success           = false;
        unsigned char* srcBits = static_cast<unsigned char*>(lr.pBits);
        int srcPitch           = lr.Pitch;

        switch (desc.Format) {
        case D3DFMT_A8R8G8B8: {
            // Direct copy for ARGB8888
            for (int y = 0; y < syy; ++y) {
                memcpy(&dstBits[y * sxx], &srcBits[y * srcPitch], sxx * 4);
            }
            success = true;
            break;
        }

        case D3DFMT_X8R8G8B8: {
            // Add alpha channel
            uint32_t* src = reinterpret_cast<uint32_t*>(srcBits);
            int srcStride = srcPitch / 4;

            for (int y = 0; y < syy; ++y) {
                for (int x = 0; x < sxx; ++x) {
                    dstBits[y * sxx + x] = src[y * srcStride + x] | 0xFF000000;
                }
            }
            success = true;
            break;
        }

        case D3DFMT_R5G6B5: {
            // Convert RGB565 to RGBA8888
            uint16_t* src = reinterpret_cast<uint16_t*>(srcBits);
            int srcStride = srcPitch / 2;

            for (int y = 0; y < syy; ++y) {
                for (int x = 0; x < sxx; ++x) {
                    uint16_t pixel       = src[y * srcStride + x];
                    unsigned char r      = ((pixel & 0x1F) << 3);
                    unsigned char g      = (((pixel >> 5) & 0x3F) << 2);
                    unsigned char b      = (((pixel >> 11) & 0x1F) << 3);
                    dstBits[y * sxx + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
                }
            }
            success = true;
            break;
        }

        case D3DFMT_X1R5G5B5: {
            // Convert RGB555 to RGBA8888
            uint16_t* src = reinterpret_cast<uint16_t*>(srcBits);
            int srcStride = srcPitch / 2;

            for (int y = 0; y < syy; ++y) {
                for (int x = 0; x < sxx; ++x) {
                    uint16_t pixel       = src[y * srcStride + x] & 0x7FFF;
                    unsigned char r      = ((pixel & 0x1F) << 3);
                    unsigned char g      = (((pixel >> 5) & 0x1F) << 3);
                    unsigned char b      = (((pixel >> 10) & 0x1F) << 3);
                    dstBits[y * sxx + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
                }
            }
            success = true;
            break;
        }

        case D3DFMT_A1R5G5B5: {
            // Convert ARGB1555 to RGBA8888
            uint16_t* src = reinterpret_cast<uint16_t*>(srcBits);
            int srcStride = srcPitch / 2;

            for (int y = 0; y < syy; ++y) {
                for (int x = 0; x < sxx; ++x) {
                    uint16_t pixel       = src[y * srcStride + x];
                    unsigned char r      = ((pixel & 0x1F) << 3);
                    unsigned char g      = (((pixel >> 5) & 0x1F) << 3);
                    unsigned char b      = (((pixel >> 10) & 0x1F) << 3);
                    unsigned char a      = (pixel >> 15) ? 0xFF : 0x00;
                    dstBits[y * sxx + x] = (a << 24) | (r << 16) | (g << 8) | b;
                }
            }
            success = true;
            break;
        }

        case D3DFMT_A4R4G4B4: {
            // Convert ARGB4444 to RGBA8888
            uint16_t* src = reinterpret_cast<uint16_t*>(srcBits);
            int srcStride = srcPitch / 2;

            for (int y = 0; y < syy; ++y) {
                for (int x = 0; x < sxx; ++x) {
                    uint16_t pixel       = src[y * srcStride + x];
                    unsigned char r      = ((pixel & 0xF) << 4);
                    unsigned char g      = (((pixel >> 4) & 0xF) << 4);
                    unsigned char b      = (((pixel >> 8) & 0xF) << 4);
                    unsigned char a      = ((pixel >> 12) << 4);
                    dstBits[y * sxx + x] = (a << 24) | (r << 16) | (g << 8) | b;
                }
            }
            success = true;
            break;
        }
        }

        dest->UnlockRect();
        dest->Release();
        destTex->Release();

        return success;
    };

    void CDevice::GetDims(const TexHandle& tex, int& sx, int& sy) const {
        sx = 0;
        sy = 0;

        if (!IsTexValid(tex))
            return;

        Texture* texMap = this->mActiveTextures.GetItem(tex.m_handle)->m_maps[0];
        TexType type    = texMap->mType;

        if (type < TT_3D_FROM_FILE) {
            sx = texMap->mSurface2D.Width;
            sy = texMap->mSurface2D.Height;
        } else if (type == TT_3D_FROM_FILE || type == TT_3D_DYNAMIC) {
            sx = texMap->mSurface3D.Width;
            sy = texMap->mSurface3D.Height;
        }
    };

    void CDevice::TexCopy(const TexHandle& dest, const TexHandle& src, bool withMips) {
        if (!IsTexValid(dest) || !IsTexValid(src))
            return;

        Texture* destTexMap = this->mActiveTextures.GetItem(dest.m_handle)->m_maps[0];
        Texture* srcTexMap  = this->mActiveTextures.GetItem(src.m_handle)->m_maps[0];

        int numLevels;
        if (withMips) {
            uint32_t srcLevels  = srcTexMap->mHandleBase->GetLevelCount();
            uint32_t destLevels = destTexMap->mHandleBase->GetLevelCount();
            numLevels           = (srcLevels < destLevels) ? srcLevels : destLevels;
        } else {
            numLevels = 1;
        }

        for (int level = 0; level < numLevels; ++level) {
            IDirect3DSurface9* srcSurf  = nullptr;
            IDirect3DSurface9* destSurf = nullptr;

            HRESULT hr = static_cast<IDirect3DTexture9*>(destTexMap->mHandleBase)->GetSurfaceLevel(level, &destSurf);
            if (FAILED(hr)) {
                LOG_INFO("cannot acquire surfaces to copy");
                break;
            }

            hr = static_cast<IDirect3DTexture9*>(srcTexMap->mHandleBase)->GetSurfaceLevel(level, &srcSurf);
            if (FAILED(hr)) {
                destSurf->Release();
                LOG_INFO("cannot acquire surfaces to copy");
                break;
            }

            hr = D3DXLoadSurfaceFromSurface(destSurf, nullptr, nullptr, srcSurf, nullptr, nullptr, D3DX_FILTER_POINT, 0);

            _SetLastResult(hr);

            if (FAILED(hr)) {
                LOG_INFO("texcopy failed, result = %d", hr);
            }

            srcSurf->Release();
            destSurf->Release();
        }
    };

    void CDevice::TexCopy(const TexHandle& a, const TexHandle& b) {
        TexCopy(a, b, false);
    };

    void CDevice::CreateMips(const TexHandle& tex) {
        if (!IsTexValid(tex))
            return;

        Texture* texMap = this->mActiveTextures.GetItem(tex.m_handle)->m_maps[0];

        IDirect3DSurface9* baseSurf = nullptr;
        HRESULT hr                  = static_cast<IDirect3DTexture9*>(texMap->mHandleBase)->GetSurfaceLevel(0, &baseSurf);
        if (FAILED(hr)) {
            LOG_INFO("cannot acquire surfaces to copy");
            return;
        }

        uint32_t numLevels = texMap->mHandleBase->GetLevelCount();
        if (numLevels <= 1) {
            baseSurf->Release();
            return;
        }

        for (uint32_t level = 1; level < numLevels; ++level) {
            IDirect3DSurface9* mipSurf = nullptr;
            hr                         = static_cast<IDirect3DTexture9*>(texMap->mHandleBase)->GetSurfaceLevel(level, &mipSurf);
            if (FAILED(hr)) {
                LOG_INFO("cannot acquire surfaces to copy");
                break;
            }

            hr = D3DXLoadSurfaceFromSurface(mipSurf, nullptr, nullptr, baseSurf, nullptr, nullptr, D3DX_FILTER_LINEAR, 0);

            _SetLastResult(hr);

            if (FAILED(hr)) {
                LOG_INFO("texcopy failed, result = %d", hr);
            }

            baseSurf->Release();
            baseSurf = mipSurf;
        }

        baseSurf->Release();
    };

    void CDevice::RepaintAllTexturesMips() {
        // Load mips texture
        TexHandle texMips = AddTexture("data/mips.dds", 2);

        for (auto [_, texture] : this->mActiveTextures) {

            if (texture->m_maps.empty())
                continue;

            Texture* texMap = texture->m_maps[0];

            if (texMap->mType != TT_2D_FROM_FILE)
                continue;

            uint32_t numLevels = texMap->mHandleBase->GetLevelCount();

            for (uint32_t level = 0; level < numLevels; ++level) {
                if (texMap->mType > TT_2D_RENDER_TARGET)
                    continue;

                IDirect3DSurface9* dstSurf = nullptr;
                HRESULT hr                 = static_cast<IDirect3DTexture9*>(texMap->mHandleBase)->GetSurfaceLevel(level, &dstSurf);
                if (FAILED(hr)) {
                    LOG_INFO("cannot acquire surfaces to copy with mip %d", level);
                    ReleaseTexture(texMips);
                    return;
                }

                int mipLevel = (level <= 9) ? level : 9;

                IDirect3DSurface9* srcSurf = nullptr;
                Texture* mipsTexMap        = mActiveTextures.GetItem(texMips.m_handle)->m_maps[0];
                hr                         = static_cast<IDirect3DTexture9*>(mipsTexMap->mHandleBase)->GetSurfaceLevel(mipLevel, &srcSurf);
                if (FAILED(hr)) {
                    dstSurf->Release();
                    LOG_INFO("cannot acquire surfaces to copy with mip %d", level);
                    ReleaseTexture(texMips);
                    return;
                }

                _SetLastResult(D3DXLoadSurfaceFromSurface(dstSurf, nullptr, nullptr, srcSurf, nullptr, nullptr, D3DX_FILTER_POINT, 0));

                dstSurf->Release();
                srcSurf->Release();
            }
        }

        ReleaseTexture(texMips);
        m_reloadAllTextures = true;
    };

    void CDevice::initFullScreenQuad() {
        m_fsQuadVb = AddVbStreaming(VERTEX_XYZT1, 4, 0);
        m_fsQuadIb = AddIb(4, false);

        uint16_t* indices = static_cast<uint16_t*>(LockIb(m_fsQuadIb, 0, 0, 0));
        indices[0]        = 0;
        indices[1]        = 1;
        indices[2]        = 2;
        indices[3]        = 3;
        UnlockIb(m_fsQuadIb);
    };

    void CDevice::doneFullScreenQuadStuff() {
        ReleaseVb(m_fsQuadVb);
        ReleaseIb(m_fsQuadIb);
    };

    void CDevice::DrawFullScreenQuadEffect(IEffect* shader) {
        float flOffsetS = 0.5f / static_cast<float>(m_curViewportD3D.Width);
        float flOffsetT = 0.5f / static_cast<float>(m_curViewportD3D.Height);

        int offset;
        float* verts = static_cast<float*>(LockVbStreaming(m_fsQuadVb, 4, offset, nullptr));

        // Vertex 0: top-right
        verts[0] = 1.0f;
        verts[1] = -1.0f;
        verts[2] = 0.0f;
        verts[3] = 1.0f + flOffsetS;
        verts[4] = 1.0f + flOffsetT;

        // Vertex 1: top-left
        verts[5] = 1.0f;
        verts[6] = 1.0f;
        verts[7] = 0.0f;
        verts[8] = 1.0f + flOffsetS;
        verts[9] = flOffsetT;

        // Vertex 2: bottom-right
        verts[10] = -1.0f;
        verts[11] = -1.0f;
        verts[12] = 0.0f;
        verts[13] = flOffsetS;
        verts[14] = 1.0f + flOffsetT;

        // Vertex 3: bottom-left
        verts[15] = -1.0f;
        verts[16] = 1.0f;
        verts[17] = 0.0f;
        verts[18] = flOffsetS;
        verts[19] = flOffsetT;

        UnlockVb(m_fsQuadVb);
        SetHandleToStream0(m_fsQuadVb);
        SetHandleIndices(m_fsQuadIb, 0);
        DrawIndexedPrimitiveEffect(M3DPT_TRIANGLESTRIP, shader, 0, 4, 0, 2);
    };

    void CDevice::DrawFullScreenQuad(bool useShader) {
        float flOffsetS = 0.5f / static_cast<float>(m_curViewportD3D.Width);
        float flOffsetT = 0.5f / static_cast<float>(m_curViewportD3D.Height);

        int offset;
        float* verts = static_cast<float*>(LockVbStreaming(m_fsQuadVb, 4, offset, nullptr));

        // Vertex 0: top-right
        verts[0] = 1.0f;
        verts[1] = -1.0f;
        verts[2] = 0.0f;
        verts[3] = 1.0f + flOffsetS;
        verts[4] = 1.0f + flOffsetT;

        // Vertex 1: top-left
        verts[5] = 1.0f;
        verts[6] = 1.0f;
        verts[7] = 0.0f;
        verts[8] = 1.0f + flOffsetS;
        verts[9] = flOffsetT;

        // Vertex 2: bottom-right
        verts[10] = -1.0f;
        verts[11] = -1.0f;
        verts[12] = 0.0f;
        verts[13] = flOffsetS;
        verts[14] = 1.0f + flOffsetT;

        // Vertex 3: bottom-left
        verts[15] = -1.0f;
        verts[16] = 1.0f;
        verts[17] = 0.0f;
        verts[18] = flOffsetS;
        verts[19] = flOffsetT;

        UnlockVb(m_fsQuadVb);
        SetHandleToStream0(m_fsQuadVb);
        SetHandleIndices(m_fsQuadIb, 0);

        if (useShader)
            DrawIndexedPrimitiveShader(M3DPT_TRIANGLESTRIP, 0, 4, 0, 2);
        else
            DrawIndexedPrimitive(M3DPT_TRIANGLESTRIP, 0, 4, 0, 2);
    };

    void CDevice::DrawFullScreenQuad() {
        DrawFullScreenQuad(true);
    };

    IbHandle CDevice::AddIb(int indexcount, bool dyn) {
        // Find free slot
        size_t freeSlot = 0;
        for (; freeSlot < m_ibs.size(); ++freeSlot) {
            if (!m_ibs[freeSlot].m_ib)
                break;
        }

        // Add new slot if needed
        if (freeSlot == m_ibs.size()) {
            CIndexBuffer ib;
            ib.m_ib     = nullptr;
            ib.mRefs    = 0;
            ib.m_locked = 0;
            m_ibs.push_back(ib);
        }

        CIndexBuffer& ib = m_ibs[freeSlot];

        ib.m_desc.Format = D3DFMT_INDEX16;
        ib.m_desc.Size   = 2 * indexcount;
        ib.m_desc.Type   = D3DRTYPE_INDEXBUFFER;

        if (dyn) {
            ib.m_desc.Pool  = D3DPOOL_DEFAULT;
            ib.m_desc.Usage = D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC;
            m_pd3dDevice->EvictManagedResources();
        } else {
            ib.m_desc.Pool  = D3DPOOL_MANAGED;
            ib.m_desc.Usage = D3DUSAGE_WRITEONLY;
        }

        HRESULT hr = m_pd3dDevice->CreateIndexBuffer(ib.m_desc.Size, ib.m_desc.Usage, ib.m_desc.Format, ib.m_desc.Pool, &ib.m_ib, nullptr);

        IbHandle result;

        if (SUCCEEDED(hr)) {
            ib.m_curPos          = 0;
            ib.mRefs             = 1;
            m_isDevMemStatsValid = false;
            result.m_handle      = static_cast<int>(freeSlot);
        } else {
            ib.m_ib         = nullptr;
            result.m_handle = -1;
        }

        return result;
    };

    IbHandle CDevice::AddIbStreaming(int sz) {
        return AddIb(sz, true);
    };

    void CDevice::SetPoolIndices(const IbPoolField& PoolField, uint32_t VertOffset) {
        SetHandleIndices(PoolField.Ib, VertOffset);
    };

    void CDevice::SetHandleIndices(const IbHandle& id, int32_t ofs) {
        if (id.m_handle < 0)
            return;

        if (static_cast<size_t>(id.m_handle) >= m_ibs.size())
            return;

        CIndexBuffer& ib = m_ibs[id.m_handle];

        if (ib.m_ib) {
            setIndices(ib.m_ib, ofs);
        }
    };

    void* CDevice::LockIb(const IbHandle& id, int sz, int ofs, uint32_t flags) {
        if (id.m_handle < 0)
            return nullptr;

        if (static_cast<size_t>(id.m_handle) >= m_ibs.size())
            return nullptr;

        CIndexBuffer& ib = m_ibs[id.m_handle];

        if (!ib.m_ib)
            return nullptr;

        // Adjust flags based on pool type
        if (ib.m_desc.Pool != D3DPOOL_DEFAULT) {
            flags &= ~(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE);
        } else if (flags == 0) {
            flags = D3DLOCK_DISCARD;
        }

        flags |= D3DLOCK_NOSYSLOCK;

        int lockSize   = sz;
        int lockOffset = ofs;

        if (sz == 0) {
            lockSize   = ib.m_desc.Size / 2;
            lockOffset = 0;
        }

        void* data = nullptr;
        HRESULT hr = ib.m_ib->Lock(2 * lockOffset, 2 * lockSize, &data, flags);

        if (FAILED(hr))
            return nullptr;

        ib.m_locked = 1;
        return data;
    };

    void* CDevice::LockIbStreaming(const IbHandle& id, int sz, int& ofs, int* discarded) {
        if (id.m_handle < 0)
            return nullptr;

        if (static_cast<size_t>(id.m_handle) >= m_ibs.size())
            return nullptr;

        CIndexBuffer& ib = m_ibs[id.m_handle];

        if (!ib.m_ib)
            return nullptr;

        ofs = ib.m_curPos;
        ib.m_curPos += sz;

        // Wrap around if we exceed buffer size
        if (ib.m_curPos >= static_cast<int>(ib.m_desc.Size / 2)) {
            ib.m_curPos = sz;
            ofs         = 0;
        }

        uint32_t flags = (ofs != 0) ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD;
        void* data     = LockIb(id, sz, ofs, flags);

        if (discarded)
            *discarded = (ofs == 0);

        return data;
    };

    void CDevice::UnlockIb(const IbHandle& id) {
        if (id.m_handle < 0)
            return;

        if (static_cast<size_t>(id.m_handle) >= m_ibs.size())
            return;

        CIndexBuffer& ib = m_ibs[id.m_handle];

        if (ib.m_ib) {
            ib.m_ib->Unlock();
            ib.m_locked = 0;
        }
    };

    int CDevice::ReferenceIb(const IbHandle& id) {
        if (id.m_handle < 0)
            return 0;

        if (static_cast<size_t>(id.m_handle) >= m_ibs.size())
            return 0;

        CIndexBuffer& ib = m_ibs[id.m_handle];

        if (!ib.m_ib)
            return 0;

        return ++ib.mRefs;
    };

    int CDevice::ReleaseIb(IbHandle& id) {
        if (id.m_handle < 0)
            return 0;

        if (static_cast<size_t>(id.m_handle) >= m_ibs.size())
            return 0;

        CIndexBuffer& ib = m_ibs[id.m_handle];

        if (!ib.m_ib)
            return 0;

        if (--ib.mRefs <= 0) {
            if (ib.m_ib) {
                ib.m_ib->Release();
                ib.m_ib = nullptr;
            }

            id.m_handle = -1;
        }

        m_isDevMemStatsValid = false;
        return ib.mRefs;
    };

    IbPoolField CDevice::AddIbPoolField(uint32_t Size) {
        IbPoolField result;

        if (Size > m_IbPoolSize) {
            LOG_INFO(" [ AddIbPoolField ] : Asked size is too big!!! ");
            result.Offset      = 0;
            result.Size        = 0;
            result.RealOffset  = 0;
            result.Ib.m_handle = -1;
            return result;
        }

        // Initialize pool if empty
        if (m_IbPoolBuffers.empty()) {
            IbHandle newIb = AddIb(m_IbPoolSize, false);
            m_IbPoolBuffers.push_back(newIb);

            // Add sentinel nodes at start and end
            PoolFieldInfo startInfo;
            startInfo.Offset = 0;
            startInfo.Size   = 0;
            m_IbPoolFields.push_back(startInfo);

            PoolFieldInfo endInfo;
            endInfo.Offset = m_IbPoolSize;
            endInfo.Size   = 0;
            m_IbPoolFields.push_back(endInfo);
        }

        // Find a gap large enough for the requested size
        auto it  = m_IbPoolFields.begin();
        auto end = std::prev(m_IbPoolFields.end());

        for (; it != end; ++it) {
            auto next         = std::next(it);
            uint32_t gapStart = it->Offset + it->Size;
            uint32_t gapEnd   = next->Offset;
            uint32_t gapSize  = gapEnd - gapStart;

            if (gapSize >= Size) {
                // Found suitable gap
                PoolFieldInfo newInfo;
                newInfo.Offset = gapStart;
                newInfo.Size   = Size;

                m_IbPoolFields.insert(next, newInfo);

                result.Offset     = gapStart;
                result.Size       = Size;
                result.RealOffset = gapStart % m_IbPoolSize;
                result.Ib         = m_IbPoolBuffers[gapStart / m_IbPoolSize];

                return result;
            }
        }

        // No suitable gap found, allocate new buffer
        uint32_t lastOffset = std::prev(m_IbPoolFields.end())->Offset;

        IbHandle newIb = AddIb(m_IbPoolSize, false);
        m_IbPoolBuffers.push_back(newIb);

        // Add new sentinel at end of new buffer
        PoolFieldInfo endInfo;
        endInfo.Offset = lastOffset + m_IbPoolSize;
        endInfo.Size   = 0;
        m_IbPoolFields.push_back(endInfo);

        // Recursively try again (original behavior)
        return AddIbPoolField(Size);
    };

    void CDevice::ReleaseIbPoolField(IbPoolField& PoolField) {
        // Find the field in the list
        auto it = m_IbPoolFields.begin();
        for (; it != m_IbPoolFields.end(); ++it) {
            if (PoolField.Offset == it->Offset && PoolField.Size == it->Size)
                break;
        }

        if (it == m_IbPoolFields.end()) {
            LOG_INFO("[ReleaseIbPoolField]: Cant find specified field in index buffer");
            return;
        }

        // Remove the field from the list
        m_IbPoolFields.erase(it);
        PoolField.Ib.m_handle = -1;

        // Check if we can free the last buffer
        auto lastSentinel = std::prev(m_IbPoolFields.end());
        auto beforeLast   = std::prev(lastSentinel);

        uint32_t gapSize = lastSentinel->Offset - beforeLast->Offset - beforeLast->Size;

        if (gapSize >= m_IbPoolSize && m_IbPoolFields.size() > 2) {
            // Free the last buffer
            if (!m_IbPoolBuffers.empty()) {
                ReleaseIb(m_IbPoolBuffers.back());
                m_IbPoolBuffers.pop_back();
                m_IbPoolFields.erase(lastSentinel);
            }
        }
    };

    void* CDevice::LockIbPoolField(const IbPoolField& PoolField) {
        return LockIb(PoolField.Ib, PoolField.Size, PoolField.RealOffset, 0);
    };

    void CDevice::UnlockIbPoolField(const IbPoolField& PoolField) {
        UnlockIb(PoolField.Ib);
    };

    bool CDevice::ReportVbsInfo(const char*) {
        // Build vertex type name map
        std::map<IDirect3DVertexDeclaration9*, std::string> typeToStr;
        typeToStr[m_vdXYZCT1]         = "XYZCT1";
        typeToStr[m_vdXYZT1]          = "XYZT1";
        typeToStr[m_vdXYZNT1]         = "XYZNT1";
        typeToStr[m_vdXYZNT2]         = "XYZNT2";
        typeToStr[m_vdXYZNT3]         = "XYZNT3";
        typeToStr[m_vdXYZN]           = "XYZN";
        typeToStr[m_vdXYZ]            = "XYZ";
        typeToStr[m_vdXYZW]           = "XYZW";
        typeToStr[m_vdXYZC]           = "XYZC";
        typeToStr[m_vdXYZNC]          = "XYZNC";
        typeToStr[m_vdXYZWCT1]        = "XYZWCT1";
        typeToStr[m_vdXYZWC]          = "XYZWC";
        typeToStr[m_vdXYZNCT1]        = "XYZNCT1";
        typeToStr[m_vdXYZNCT2]        = "XYZNCT2";
        typeToStr[m_vdXYZCT2]         = "XYZCT2";
        typeToStr[m_vdXYZW4NCT1]      = "XYZW4NCT1";
        typeToStr[m_vdXYZW4TNCT1]     = "XYZW4TNCT1";
        typeToStr[m_vdXYZNT1T]        = "XYZNT1T";
        typeToStr[m_vdXYZNCT1T]       = "XYZNCT1T";
        typeToStr[m_vdXYZCT1_UVW]     = "XYZCT1_UVW";
        typeToStr[m_vdXYZCT2_UVW]     = "XYZCT2_UVW";
        typeToStr[m_vdXYZNCT1_UV2_S1] = "XYZNCT1_UV2_S1";
        typeToStr[m_vdWaterTest]      = "WaterTest";
        typeToStr[m_vdGrassTest]      = "GrassTest";
        typeToStr[m_vdImpostorTest]   = "ImpostorTest";
        typeToStr[m_vdYNI]            = "YNI";
        typeToStr[m_vdXYZT1I]         = "XYZT1I";
        typeToStr[m_vdInstanceId]     = "InstanceId";

        // Collect VBs from pools
        std::vector<CVertexBuffer*> vbPools;
        for (auto& poolBuffers : m_VbPoolBuffers) {
            for (auto& vbHandle : poolBuffers) {
                if (vbHandle.m_handle >= 0 && static_cast<size_t>(vbHandle.m_handle) < m_vbs.size()) {
                    CVertexBuffer& vb = m_vbs[vbHandle.m_handle];
                    if (vb.m_vb)
                        vbPools.push_back(&vb);
                }
            }
        }

        // Collect native VBs (non-pool)
        std::vector<CVertexBuffer*> vbs;
        uint32_t dynamicVbSize = 0;
        uint32_t staticVbSize  = 0;
        uint32_t vbPoolsSize   = 0;

        for (auto& vb : m_vbs) {
            if (!vb.m_vb)
                continue;

            // Check if in pool
            bool inPool = (std::find(vbPools.begin(), vbPools.end(), &vb) != vbPools.end());

            if (inPool) {
                vbPoolsSize += vb.m_desc.Size;
            } else {
                vbs.push_back(&vb);
                if (vb.m_desc.Usage & D3DUSAGE_DYNAMIC)
                    dynamicVbSize += vb.m_desc.Size;
                else
                    staticVbSize += vb.m_desc.Size;
            }
        }

        // Report general stats
        LOG_INFO("** Vertex Buffer Stats **");
        LOG_INFO("Vertex Buffers count: %u", vbs.size() + vbPools.size());
        LOG_INFO("Native vertex buffers count: %u", vbs.size());
        LOG_INFO("Vertex buffers used in pools count: %u", vbPools.size());
        LOG_INFO("Total size of dynamic native VBs: %u (%.2f MB)", dynamicVbSize, dynamicVbSize / (1024.0f * 1024.0f));
        LOG_INFO("Total size of static native VBs: %u (%.2f MB)", staticVbSize, staticVbSize / (1024.0f * 1024.0f));
        LOG_INFO("Total size of VB pools: %u (%.2f MB)", vbPoolsSize, vbPoolsSize / (1024.0f * 1024.0f));
        LOG_INFO(
            "Total size of all VBs: %u (%.2f MB)",
            dynamicVbSize + staticVbSize + vbPoolsSize,
            (dynamicVbSize + staticVbSize + vbPoolsSize) / (1024.0f * 1024.0f)
        );

        return true;
    };

    VbHandle CDevice::AddVb(VertexType tvert, int vertcount, const hta::CStr& vbName, uint32_t flags) {
        // Find free slot
        size_t freeSlot = 0;
        for (; freeSlot < m_vbs.size(); ++freeSlot) {
            if (!m_vbs[freeSlot].m_vb)
                break;
        }

        // Add new slot if needed
        if (freeSlot == m_vbs.size()) {
            CVertexBuffer vb;
            vb.m_vb              = nullptr;
            vb.mRefs             = 0;
            vb.m_locked          = 0;
            vb.m_lockedAtPresent = -1;
            vb.m_vertexDecl      = nullptr;
            vb.m_vbName          = "";
            vb.m_vtType          = tvert;
            m_vbs.push_back(vb);
        }

        CVertexBuffer& vb = m_vbs[freeSlot];

        // Get vertex info
        uint32_t fvf;
        int vertSize;
        IDirect3DVertexDeclaration9* vdecl;
        GetVertexInfo(tvert, fvf, vertSize, vdecl);

        vb.m_desc.Format = D3DFMT_VERTEXDATA;
        vb.m_desc.Type   = D3DRTYPE_VERTEXBUFFER;
        vb.m_desc.Size   = vertcount * vertSize;
        vb.m_desc.FVF    = fvf;
        vb.m_vertSz      = vertSize;
        vb.m_vtType      = tvert;

        if (flags & D3DUSAGE_DYNAMIC) {
            vb.m_desc.Pool  = D3DPOOL_DEFAULT;
            vb.m_desc.Usage = D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC;
            m_pd3dDevice->EvictManagedResources();
        } else {
            vb.m_desc.Pool  = D3DPOOL_MANAGED;
            vb.m_desc.Usage = D3DUSAGE_WRITEONLY;
        }

        HRESULT hr = m_pd3dDevice->CreateVertexBuffer(vb.m_desc.Size, vb.m_desc.Usage, vb.m_desc.FVF, vb.m_desc.Pool, &vb.m_vb, nullptr);

        VbHandle result;

        if (SUCCEEDED(hr)) {
            vb.m_curPos          = 0;
            vb.mRefs             = 1;
            vb.m_vertexDecl      = vdecl;
            vb.m_vbName          = vbName;
            m_isDevMemStatsValid = false;
            result.m_handle      = static_cast<int>(freeSlot);
        } else {
            vb.m_vb         = nullptr;
            result.m_handle = -1;
        }

        return result;
    };

    VbHandle CDevice::AddVbStreaming(VertexType tv, int sz, uint32_t flags) {
        flags |= D3DUSAGE_DYNAMIC;
        return AddVb(tv, sz, "Streaming", flags);
    };

    void CDevice::SetPoolToStream0(const VbPoolField& PoolField) {
        SetHandleToStream(0, PoolField.Vb);
    };

    void CDevice::SetHandleToStream0(const VbHandle& id) {
        SetHandleToStream(0, id);
    };

    void CDevice::SetPoolToStream(int Stream, const VbPoolField& PoolField) {
        SetHandleToStream(Stream, PoolField.Vb);
    };

    void CDevice::SetHandleToStream(int32_t stream, const VbHandle& id) {
        if (id.m_handle < 0)
            return;

        if (static_cast<size_t>(id.m_handle) >= m_vbs.size())
            return;

        CVertexBuffer& vb = m_vbs[id.m_handle];

        if (this->mBindedStreams[stream] == vb.m_vb)
            return;
        else
            this->mBindedStreams[stream] = vb.m_vb;

        static wchar_t marker[256];
        swprintf(
            marker,
            256,
            L"SetStream %d: %S (handle=%d, decl=%p, vtype=%d, stride=%d)",
            stream,
            vb.m_vbName.m_charPtr,
            id.m_handle,
            vb.m_vertexDecl,
            vb.m_vtType,
            vb.m_vertSz
        );
        D3DPERF_SetMarker(D3DCOLOR_XRGB(0, 255, 0), marker);

        if (vb.m_vb) {
            if (stream == 0 && vb.m_vertexDecl)
                setVertexDeclaration(vb.m_vertexDecl);

            _SetLastResult(setStreamSource(stream, vb.m_vb, vb.m_vertSz));
        } else {
            D3DPERF_SetMarker(D3DCOLOR_XRGB(255, 0, 0), L"ERROR: VB is NULL!");
        }
    };

    void CDevice::SetToStreams01(const VbHandle& id0, const VbHandle& id1) {
        if (id0.m_handle < 0 || static_cast<size_t>(id0.m_handle) >= m_vbs.size())
            return;

        CVertexBuffer& vb0 = m_vbs[id0.m_handle];
        if (!vb0.m_vb)
            return;

        if (id1.m_handle < 0 || static_cast<size_t>(id1.m_handle) >= m_vbs.size())
            return;

        CVertexBuffer& vb1 = m_vbs[id1.m_handle];
        if (!vb1.m_vb)
            return;

        // Look up combined vertex declaration
        auto key = std::make_pair(vb0.m_vertexDecl, vb1.m_vertexDecl);
        auto it  = m_combinedVD.find(key);

        if (it != m_combinedVD.end()) {
            IDirect3DVertexDeclaration9* combinedDecl = it->second;
            if (combinedDecl)
                setVertexDeclaration(combinedDecl);
        }

        setStreamSource(0, vb0.m_vb, vb0.m_vertSz);
        setStreamSource(1, vb1.m_vb, vb1.m_vertSz);
    };

    void* CDevice::LockVb(const VbHandle& id, int sz, int ofs, uint32_t flags) {
        if (id.m_handle < 0)
            return nullptr;

        if (static_cast<size_t>(id.m_handle) >= m_vbs.size())
            return nullptr;

        CVertexBuffer& vb = m_vbs[id.m_handle];

        if (!vb.m_vb)
            return nullptr;

        // Adjust flags based on pool type
        if (vb.m_desc.Pool != D3DPOOL_DEFAULT) {
            flags &= ~(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE);
        } else if (flags == 0) {
            flags = D3DLOCK_DISCARD;
        }

        flags |= D3DLOCK_NOSYSLOCK;

        int lockSize   = sz;
        int lockOffset = ofs;

        if (sz == 0) {
            lockSize   = vb.m_desc.Size / vb.m_vertSz;
            lockOffset = 0;
        }

        void* data = nullptr;
        HRESULT hr = vb.m_vb->Lock(lockOffset * vb.m_vertSz, lockSize * vb.m_vertSz, &data, flags);

        if (FAILED(hr))
            return nullptr;

        vb.m_locked = 1;
        return data;
    };

    void* CDevice::LockVbStreaming(const VbHandle& id, int sz, int& ofs, int* discarded) {
        if (id.m_handle < 0)
            return nullptr;

        if (static_cast<size_t>(id.m_handle) >= m_vbs.size())
            return nullptr;

        CVertexBuffer& vb = m_vbs[id.m_handle];

        if (!vb.m_vb)
            return nullptr;

        int maxVerts = vb.m_desc.Size / vb.m_vertSz;

        ofs = vb.m_curPos;
        vb.m_curPos += sz;

        // Wrap around if we exceed buffer size
        if (vb.m_curPos >= maxVerts) {
            vb.m_curPos = sz;
            ofs         = 0;
        }

        uint32_t flags = (ofs != 0) ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD;
        void* data     = LockVb(id, sz, ofs, flags);

        if (discarded)
            *discarded = (ofs == 0);

        return data;
    };

    void CDevice::UnlockVb(const VbHandle& id) {
        if (id.m_handle < 0)
            return;

        if (static_cast<size_t>(id.m_handle) >= m_vbs.size())
            return;

        CVertexBuffer& vb = m_vbs[id.m_handle];

        if (vb.m_vb) {
            vb.m_vb->Unlock();
            vb.m_locked = 0;
        }
    };

    int CDevice::ReferenceVb(const VbHandle& id) {
        if (id.m_handle < 0)
            return 0;

        if (static_cast<size_t>(id.m_handle) >= m_vbs.size())
            return 0;

        CVertexBuffer& vb = m_vbs[id.m_handle];

        if (!vb.m_vb)
            return 0;

        return ++vb.mRefs;
    };

    int CDevice::ReleaseVb(VbHandle& id) {
        if (id.m_handle < 0)
            return 0;

        if (static_cast<size_t>(id.m_handle) >= m_vbs.size())
            return 0;

        CVertexBuffer& vb = m_vbs[id.m_handle];

        if (!vb.m_vb)
            return 0;

        if (--vb.mRefs <= 0) {
            if (vb.m_vb) {
                vb.m_vb->Release();
                vb.m_vb = nullptr;
            }

            id.m_handle = -1;
        }

        vb.m_lockedAtPresent = -1;
        m_isDevMemStatsValid = false;

        return vb.mRefs;
    };

    bool CDevice::ReportIbsInfo(const char*) {
        // Collect IBs from pools
        std::vector<CIndexBuffer*> ibPools;
        for (auto& ibHandle : m_IbPoolBuffers) {
            if (ibHandle.m_handle >= 0 && static_cast<size_t>(ibHandle.m_handle) < m_ibs.size()) {
                CIndexBuffer& ib = m_ibs[ibHandle.m_handle];
                if (ib.m_ib)
                    ibPools.push_back(&ib);
            }
        }

        // Collect native IBs (non-pool)
        std::vector<CIndexBuffer*> ibs;
        uint32_t dynamicIbSize = 0;
        uint32_t staticIbSize  = 0;
        uint32_t ibPoolsSize   = 0;

        for (auto& ib : m_ibs) {
            if (!ib.m_ib)
                continue;

            // Check if in pool
            bool inPool = (std::find(ibPools.begin(), ibPools.end(), &ib) != ibPools.end());

            if (inPool) {
                ibPoolsSize += ib.m_desc.Size;
            } else {
                ibs.push_back(&ib);
                if (ib.m_desc.Usage & D3DUSAGE_DYNAMIC)
                    dynamicIbSize += ib.m_desc.Size;
                else
                    staticIbSize += ib.m_desc.Size;
            }
        }

        // Calculate pool usage
        uint32_t poolUsedSize = 0;
        for (auto& fieldInfo : m_IbPoolFields) {
            poolUsedSize += fieldInfo.Size * 2; // indices are 16-bit
        }

        // Report general stats
        LOG_INFO("** Index Buffer Stats **");
        LOG_INFO("Index buffers count: %u", ibs.size() + ibPools.size());
        LOG_INFO("Native index buffers count: %u", ibs.size());
        LOG_INFO("Index buffers used in pools count: %u", ibPools.size());
        LOG_INFO("Total size of dynamic native IBs: %u (%.2f MB)", dynamicIbSize, dynamicIbSize / (1024.0f * 1024.0f));
        LOG_INFO("Total size of static native IBs: %u (%.2f MB)", staticIbSize, staticIbSize / (1024.0f * 1024.0f));
        LOG_INFO("Total pool size: %u (%.2f MB)", ibPoolsSize, ibPoolsSize / (1024.0f * 1024.0f));
        LOG_INFO("Total used size in pool: %u (%.2f MB)", poolUsedSize, poolUsedSize / (1024.0f * 1024.0f));
        LOG_INFO(
            "Total size of all IBs: %u (%.2f MB)",
            dynamicIbSize + staticIbSize + ibPoolsSize,
            (dynamicIbSize + staticIbSize + ibPoolsSize) / (1024.0f * 1024.0f)
        );

        return true;
    };

    VbPoolField CDevice::AddVbPoolField(VertexType Type, uint32_t Size) {
        VbPoolField result;

        if (Size > m_VbPoolSize) {
            LOG_INFO(" [ AddVbPoolField ] : Asked size is too big!!! ");
            result.Offset      = 0;
            result.Size        = 0;
            result.RealOffset  = 0;
            result.VertType    = Type;
            result.Vb.m_handle = -1;
            return result;
        }

        // Find or create pool for this vertex type
        size_t poolIndex = 0;
        bool foundPool   = false;

        for (; poolIndex < m_VbPoolTypes.size(); ++poolIndex) {
            if (m_VbPoolTypes[poolIndex] == Type) {
                foundPool = true;
                break;
            }
        }

        // Initialize pool if needed
        if (!foundPool) {
            m_VbPoolTypes.push_back(Type);

            std::vector<VbHandle> newPool;
            VbHandle newVb = AddVb(Type, m_VbPoolSize, "Pool", 0);
            newPool.push_back(newVb);
            m_VbPoolBuffers.push_back(newPool);

            std::list<PoolFieldInfo> newFieldList;

            // Add sentinel at start
            PoolFieldInfo startInfo;
            startInfo.Offset = 0;
            startInfo.Size   = 0;
            newFieldList.push_back(startInfo);

            // Add sentinel at end
            PoolFieldInfo endInfo;
            endInfo.Offset = m_VbPoolSize;
            endInfo.Size   = 0;
            newFieldList.push_back(endInfo);

            m_VbPoolFields.push_back(newFieldList);
            poolIndex = m_VbPoolTypes.size() - 1;
        }

        // Find a gap large enough for the requested size
        auto& fieldList = m_VbPoolFields[poolIndex];
        auto it         = fieldList.begin();
        auto end        = std::prev(fieldList.end());

        for (; it != end; ++it) {
            auto next         = std::next(it);
            uint32_t gapStart = it->Offset + it->Size;
            uint32_t gapEnd   = next->Offset;
            uint32_t gapSize  = gapEnd - gapStart;

            if (gapSize >= Size) {
                // Found suitable gap
                PoolFieldInfo newInfo;
                newInfo.Offset = gapStart;
                newInfo.Size   = Size;

                fieldList.insert(next, newInfo);

                result.Offset     = gapStart;
                result.Size       = Size;
                result.RealOffset = gapStart % m_VbPoolSize;
                result.VertType   = Type;
                result.Vb         = m_VbPoolBuffers[poolIndex][gapStart / m_VbPoolSize];

                return result;
            }
        }

        // No suitable gap found, allocate new buffer
        uint32_t lastOffset = std::prev(fieldList.end())->Offset;

        VbHandle newVb = AddVb(Type, m_VbPoolSize, "Pool", 0);
        m_VbPoolBuffers[poolIndex].push_back(newVb);

        // Add new sentinel at end of new buffer
        PoolFieldInfo endInfo;
        endInfo.Offset = lastOffset + m_VbPoolSize;
        endInfo.Size   = 0;
        fieldList.push_back(endInfo);

        // Recursively try again (original behavior)
        return AddVbPoolField(Type, Size);
    };

    void CDevice::ReleaseVbPoolField(VbPoolField& PoolField) {
        // Find the pool for this vertex type
        size_t poolIndex = 0;
        bool foundPool   = false;

        for (; poolIndex < m_VbPoolTypes.size(); ++poolIndex) {
            if (m_VbPoolTypes[poolIndex] == PoolField.VertType) {
                foundPool = true;
                break;
            }
        }

        if (!foundPool) {
            LOG_INFO("[ReleaseVbPoolField]: Cant find specified vertex buffer");
            return;
        }

        // Find the field in the list
        auto& fieldList = m_VbPoolFields[poolIndex];
        auto it         = fieldList.begin();

        for (; it != fieldList.end(); ++it) {
            if (it->Offset == PoolField.Offset && it->Size == PoolField.Size)
                break;
        }

        if (it == fieldList.end()) {
            LOG_INFO("[ReleaseVbPoolField]: Cant find specified field in vertex buffer");
            return;
        }

        // Remove the field from the list
        fieldList.erase(it);
        PoolField.Vb.m_handle = -1;

        // Check if we can free the last buffer
        auto lastSentinel = std::prev(fieldList.end());
        auto beforeLast   = std::prev(lastSentinel);

        uint32_t gapSize = lastSentinel->Offset - beforeLast->Offset - beforeLast->Size;

        if (gapSize >= m_VbPoolSize && fieldList.size() > 2) {
            // Free the last buffer
            if (!m_VbPoolBuffers[poolIndex].empty()) {
                ReleaseVb(m_VbPoolBuffers[poolIndex].back());
                m_VbPoolBuffers[poolIndex].pop_back();
                fieldList.erase(lastSentinel);
            }
        }
    };

    void* CDevice::LockVbPoolField(const VbPoolField& PoolField) {
        return LockVb(PoolField.Vb, PoolField.Size, PoolField.RealOffset, 0);
    };

    void CDevice::UnlockVbPoolField(const VbPoolField& PoolField) {
        UnlockVb(PoolField.Vb);
    };

    VbHandle CDevice::GetVbStreaming(VertexType VertType) {
        VbHandle result;

        switch (VertType) {
        case VERTEX_XYZ:
            result = m_vbXyz;
            break;
        case VERTEX_XYZC:
            result = m_vbXyzc;
            break;
        case VERTEX_XYZWCT1:
            result = m_vbXyzwct1;
            break;
        case VERTEX_XYZNC:
            result = m_vbXyznc;
            break;
        case VERTEX_XYZCT1:
            result = m_vbXyzct1;
            break;
        case VERTEX_XYZNT1:
            result = m_vbXyznt1;
            break;
        case VERTEX_XYZNCT1:
            result = m_vbXyznct1;
            break;
        case VERTEX_XYZNCT2:
            result = m_vbXyznct2;
            break;
        case VERTEX_XYZNT2:
            result = m_vbXyznt2;
            break;
        case VERTEX_XYZNT3:
            result = m_vbXyznt3;
            break;
        case VERTEX_XYZNT1T:
            result = m_vbXyznt1t;
            break;
        case VERTEX_XYZNCT1T:
            result = m_vbXyznct1t;
            break;
        default:
            LOG_INFO("Unsupported vertex type in GetVbStreaming: %d", VertType);
            result.m_handle = -1;
            break;
        }

        return result;
    };

    static D3DPRIMITIVETYPE m3dPtToD3dPt[6]{
        D3DPT_POINTLIST,
        D3DPT_LINELIST,
        D3DPT_LINESTRIP,
        D3DPT_TRIANGLELIST,
        D3DPT_TRIANGLESTRIP,
        D3DPT_TRIANGLEFAN,
    };

    int32_t
    CDevice::DrawIndexedPrimitive(PrimType Type, uint32_t MinIndex, uint32_t NumVertices, uint32_t StartIndex, uint32_t PrimitiveCount) {
        setVertexShader(nullptr);
        setPixelShader(nullptr);
        ActuateStates(true);

        m_curIbBaseIdx = m_latchedIbBaseIdx;

        this->mVSUniforms.Commit();
        this->mFSUniforms.Commit();
        HRESULT hr =
            m_pd3dDevice->DrawIndexedPrimitive(m3dPtToD3dPt[Type], m_latchedIbBaseIdx, MinIndex, NumVertices, StartIndex, PrimitiveCount);

        // m_stats.polyCount += PrimitiveCount;
        //++m_stats.DIPs;
        _SetLastResult(hr);

        return SUCCEEDED(hr);
    };

    int32_t CDevice::DrawPrimitive(PrimType PrimitiveType, uint32_t StartVertex, uint32_t PrimitiveCount) {
        setVertexShader(nullptr);
        setPixelShader(nullptr);
        ActuateStates(true);

        this->mVSUniforms.Commit();
        this->mFSUniforms.Commit();
        HRESULT hr = m_pd3dDevice->DrawPrimitive(m3dPtToD3dPt[PrimitiveType], StartVertex, PrimitiveCount);

        // m_stats.polyCount += PrimitiveCount;
        //++m_stats.DPs;
        _SetLastResult(hr);

        return SUCCEEDED(hr);
    };

    int CDevice::GetMaxLights() const {
        return m_maxLights;
    };

    void CDevice::LightEnable(int numLight, int32_t state) {
        if (numLight < 8) {
            m_lightsEnabled[numLight] = state;
            m_pd3dDevice->LightEnable(numLight, state);
        }
    };

    int32_t CDevice::LightSet(int32_t numLight, const D3DLIGHT9& l) {
        if (numLight >= 8)
            return E_INVALIDARG;

        m_lights[numLight] = l;
        return m_pd3dDevice->SetLight(numLight, &l);
    };

    static D3DLIGHTTYPE m3dLightTypeToD3dLightType[3]{
        D3DLIGHT_POINT,
        D3DLIGHT_SPOT,
        D3DLIGHT_DIRECTIONAL,
    };

    void CDevice::LightSet(int numLight, const LightSource& l) {
        if (numLight >= 8)
            return;

        D3DLIGHT9 light;
        light.Type         = m3dLightTypeToD3dLightType[l.m_type];
        light.Diffuse      = *reinterpret_cast<const D3DCOLORVALUE*>(&l.m_diffuse);
        light.Specular     = *reinterpret_cast<const D3DCOLORVALUE*>(&l.m_specular);
        light.Ambient      = *reinterpret_cast<const D3DCOLORVALUE*>(&l.m_ambient);
        light.Position     = *reinterpret_cast<const D3DVECTOR*>(&l.m_origin);
        light.Direction.x  = l.m_direction.x;
        light.Direction.y  = l.m_direction.y;
        light.Direction.z  = l.m_direction.z;
        light.Range        = l.m_range;
        light.Falloff      = l.m_falloff;
        light.Attenuation0 = l.m_attenuation0;
        light.Attenuation1 = l.m_attenuation1;
        light.Attenuation2 = l.m_attenuation2;
        light.Theta        = l.m_theta;
        light.Phi          = l.m_phi;

        m_lights[numLight] = light;
        m_pd3dDevice->SetLight(numLight, &light);
    };

    void CDevice::MaterialSet(const Material& mat) {
        D3DMATERIAL9 d3dMat;
        d3dMat.Diffuse.r = mat.m_diffuse.r;
        d3dMat.Diffuse.g = mat.m_diffuse.g;
        d3dMat.Diffuse.b = mat.m_diffuse.b;
        d3dMat.Diffuse.a = mat.m_diffuse.a;
        d3dMat.Ambient   = *reinterpret_cast<const D3DCOLORVALUE*>(&mat.m_ambient);
        d3dMat.Specular  = *reinterpret_cast<const D3DCOLORVALUE*>(&mat.m_specular);
        d3dMat.Emissive  = *reinterpret_cast<const D3DCOLORVALUE*>(&mat.m_emissive);
        d3dMat.Power     = mat.m_specularPower;

        _SetLastResult(m_pd3dDevice->SetMaterial(&d3dMat));
    };

    void CDevice::RelToAbs(float& x, float& y) const {
        x = m_d3dsdBackBuffer.Width * x / 1024.0f;
        y = m_d3dsdBackBuffer.Height * y / 768.0f;
    };

    void CDevice::AbsToRel(float& x, float& y) const {
        x = x / m_d3dsdBackBuffer.Width * 1024.0f;
        y = y / m_d3dsdBackBuffer.Height * 768.0f;
    };

    int32_t CDevice::AddTextureFromBackBufferHandle(TexHandle srcTex) {
        Sampler* texture = this->mActiveTextures.GetItem(srcTex.m_handle);

        if (!texture)
            return false;

        IDirect3DBaseTexture9* baseTex = texture->m_maps[0]->mHandleBase;

        IDirect3DSurface9* pDstSurf = nullptr;
        HRESULT hr                  = static_cast<IDirect3DTexture9*>(baseTex)->GetSurfaceLevel(0, &pDstSurf);
        if (FAILED(hr))
            return false;

        IDirect3DSurface9* pSrcSurf = nullptr;
        hr                          = m_pd3dDevice->GetRenderTarget(0, &pSrcSurf);

        if (SUCCEEDED(hr)) {
            hr = D3DXLoadSurfaceFromSurface(pDstSurf, nullptr, nullptr, pSrcSurf, nullptr, nullptr, D3DX_FILTER_POINT, 0);
            pSrcSurf->Release();
        }

        pDstSurf->Release();
        return SUCCEEDED(hr);
    };

    TexHandle CDevice::AddTextureFromBackBuffer(int width, int height) {
        TexHandle tex = AddDynamicTexture("$TexFromBackBuf", width, height, 1);

        if (tex.m_handle < 0) {
            tex.m_handle = -1;
            return tex;
        }

        SetTextureParameter(tex, TM_WRAP_S, 3);
        SetTextureParameter(tex, TM_WRAP_T, 3);

        if (!AddTextureFromBackBufferHandle(tex)) {
            ReleaseTexture(tex);
            tex.m_handle = -1;
            return tex;
        }

        return tex;
    };

    void CDevice::ScreenShot(const char* fileName, int width, int height) {
        IDirect3DSurface9* pBackBuffer = nullptr;
        HRESULT hr                     = m_pd3dDevice->GetRenderTarget(0, &pBackBuffer);
        if (FAILED(hr))
            return;

        std::string finalFileName;

        if (!fileName || strlen(fileName) == 0) {
            // Generate unique filename
            int counter = 0;
            char buffer[256];
            do {
                snprintf(buffer, sizeof(buffer), "screen%05u.png", counter++);
                finalFileName = buffer;
            } while (GetFileAttributesA(finalFileName.c_str()) != INVALID_FILE_ATTRIBUTES);
        } else {
            finalFileName = fileName;
        }

        // Use config dimensions if not specified
        auto& cfg = hta::m3d::Kernel::Instance()->GetEngineCfg();
        if (width == -1)
            width = cfg.m_r_width.m_i;
        if (height == -1)
            height = cfg.m_r_height.m_i;

        TexHandle tex = AddTextureFromBackBuffer(width, height);
        if (tex.m_handle >= 0) {
            SaveTextureToFile(tex, finalFileName.c_str(), M3DIFF_PNG);
            ReleaseTexture(tex);
        }

        pBackBuffer->Release();
    }

    HRESULT SaveSurfaceToTGAFile(IDirect3DDevice9* pD3DDevice, const char* szFileName, IDirect3DSurface9* pSurface, int width, int height) {
        D3DSURFACE_DESC desc;
        pSurface->GetDesc(&desc);

        // Override dimensions if specified
        if (width != -1)
            desc.Width = width;
        if (height != -1)
            desc.Height = height;

        // Create scratch surface for reading
        IDirect3DSurface9* pTempSurface = nullptr;
        HRESULT hr = pD3DDevice->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SCRATCH, &pTempSurface, nullptr);

        if (FAILED(hr))
            return hr;

        // Copy surface data
        D3DXLoadSurfaceFromSurface(pTempSurface, nullptr, nullptr, pSurface, nullptr, nullptr, D3DX_FILTER_POINT, 0);

        // Lock surface for reading
        D3DLOCKED_RECT lockedRect;
        hr = pTempSurface->LockRect(&lockedRect, nullptr, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK);
        if (FAILED(hr)) {
            pTempSurface->Release();
            return hr;
        }

        // Open file
        FILE* file = fopen(szFileName, "wb");
        if (!file) {
            pTempSurface->UnlockRect();
            pTempSurface->Release();
            return E_FAIL;
        }

        setvbuf(file, nullptr, _IOFBF, 65536);

        // Write TGA header
        uint8_t header[18]      = {0};
        header[2]               = 2; // Uncompressed RGB
        *(uint16_t*)&header[12] = (uint16_t)desc.Width;
        *(uint16_t*)&header[14] = (uint16_t)desc.Height;

        if (desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_A1R5G5B5)
            header[16] = 32; // 32-bit with alpha
        else
            header[16] = 24; // 24-bit no alpha

        header[17] = 32; // Origin at top-left

        fwrite(header, 18, 1, file);

        // Write pixel data
        uint8_t* srcRow = static_cast<uint8_t*>(lockedRect.pBits);

        for (uint32_t y = 0; y < desc.Height; ++y) {
            uint8_t* src = srcRow;

            switch (desc.Format) {
            case D3DFMT_A8R8G8B8:
                fwrite(src, desc.Width, 4, file);
                break;

            case D3DFMT_X8R8G8B8:
                for (uint32_t x = 0; x < desc.Width; ++x) {
                    fputc(src[0], file); // B
                    fputc(src[1], file); // G
                    fputc(src[2], file); // R
                    src += 4;
                }
                break;

            case D3DFMT_R5G6B5:
                for (uint32_t x = 0; x < desc.Width; ++x) {
                    uint16_t pixel = *(uint16_t*)src;
                    fputc((pixel & 0x1F) << 3, file);         // B
                    fputc(((pixel >> 5) & 0x3F) << 2, file);  // G
                    fputc(((pixel >> 11) & 0x1F) << 3, file); // R
                    src += 2;
                }
                break;

            case D3DFMT_X1R5G5B5:
                for (uint32_t x = 0; x < desc.Width; ++x) {
                    uint16_t pixel = *(uint16_t*)src;
                    fputc((pixel & 0x1F) << 3, file);         // B
                    fputc(((pixel >> 5) & 0x1F) << 3, file);  // G
                    fputc(((pixel >> 10) & 0x1F) << 3, file); // R
                    src += 2;
                }
                break;

            case D3DFMT_A1R5G5B5:
                for (uint32_t x = 0; x < desc.Width; ++x) {
                    uint16_t pixel = *(uint16_t*)src;
                    fputc((pixel & 0x1F) << 3, file);            // B
                    fputc(((pixel >> 5) & 0x1F) << 3, file);     // G
                    fputc(((pixel >> 10) & 0x1F) << 3, file);    // R
                    fputc((pixel & 0x8000) ? 0xFF : 0x00, file); // A
                    src += 2;
                }
                break;
            }

            srcRow += lockedRect.Pitch;
        }

        fclose(file);
        pTempSurface->UnlockRect();
        pTempSurface->Release();

        return S_OK;
    };

    int32_t CDevice::SaveTextureToTgaFile(TexHandle tex, const char* fileName) {
        Sampler* texture = this->mActiveTextures.GetItem(tex.m_handle);

        if (!texture)
            return false;

        IDirect3DBaseTexture9* baseTex = texture->m_maps[0]->mHandleBase;

        IDirect3DSurface9* srcSurf = nullptr;
        HRESULT hr                 = static_cast<IDirect3DTexture9*>(baseTex)->GetSurfaceLevel(0, &srcSurf);
        if (FAILED(hr))
            return false;

        bool result = SUCCEEDED(SaveSurfaceToTGAFile(m_pd3dDevice, fileName, srcSurf, -1, -1));
        srcSurf->Release();

        return result;
    };

    int32_t CDevice::SaveTextureToFile(TexHandle tex, const char* fileName, ImageFileFormats format) {
        if (!IsTexValid(tex))
            return false;

        Sampler* texture               = this->mActiveTextures.GetItem(tex.m_handle);
        IDirect3DBaseTexture9* baseTex = texture->m_maps[0]->mHandleBase;

        HRESULT hr = D3DXSaveTextureToFileA(fileName, static_cast<D3DXIMAGE_FILEFORMAT>(format), baseTex, nullptr);

        return SUCCEEDED(hr);
    };

    char* CDevice::GetCurBppStr(int* bpp) const {
        static char sstr[64];

        std::string formatStr = getD3dFmtStr(m_d3dsdBackBuffer.Format);
        strncpy(sstr, formatStr.c_str(), sizeof(sstr) - 1);
        sstr[sizeof(sstr) - 1] = '\0';

        if (bpp) {
            switch (m_d3dsdBackBuffer.Format) {
            case D3DFMT_R8G8B8:
            case D3DFMT_A8R8G8B8:
            case D3DFMT_X8R8G8B8:
                *bpp = 32;
                break;

            case D3DFMT_R5G6B5:
            case D3DFMT_X1R5G5B5:
            case D3DFMT_A1R5G5B5:
            case D3DFMT_A4R4G4B4:
            case D3DFMT_X4R4G4B4:
                *bpp = 16;
                break;

            default:
                *bpp = 0;
                break;
            }
        }

        return sstr;
    };

    char* CDevice::GetLastErrorStr() const {
        if (SUCCEEDED(m_lastResult))
            return nullptr;

        static char sstr[256];

        std::string errorStr = getD3dErrorStr(m_lastResult);
        strncpy(sstr, errorStr.c_str(), sizeof(sstr) - 1);
        sstr[sizeof(sstr) - 1] = '\0';

        return sstr;
    };

    void CDevice::ForceRecalcClipPlanes() {
        const hta::CMatrix& proj = MatGetProj();
        const hta::CMatrix& view = GetViewMatrix();

        // Calculate view-projection matrix and get inverse-transpose for plane transform
        hta::CMatrix worldToProjInvTransposed = (view * proj).PlaneTransform();

        // Transform all user clip planes from world to projection space
        uint32_t maxPlanes = GetMaxClipPlanes();
        for (uint32_t i = 0; i < maxPlanes; ++i) {
            const D3DXPLANE& worldPlane = m_userClipPlanesWorld[i];
            D3DXPLANE& projPlane        = m_userClipPlanesProj[i];

            // Transform plane: P' = M^-T * P
            hta::CVector4 planeVec(worldPlane.a, worldPlane.b, worldPlane.c, worldPlane.d);
            hta::CVector4 transformed = worldToProjInvTransposed * planeVec;

            // Unpack back to D3DXPLANE
            projPlane.a = transformed.x;
            projPlane.b = transformed.y;
            projPlane.c = transformed.z;
            projPlane.d = transformed.w;
        }
    };

    uint32_t CDevice::GetMaxClipPlanes() const {
        return m_d3dCaps.MaxUserClipPlanes;
    };

    void CDevice::SetClipPlane(int index, const hta::CPlane& plane) {
        if (index < 0 || index >= 6)
            return;

        float a = plane.m_normal.x;
        float b = plane.m_normal.y;
        float c = plane.m_normal.z;
        float d = -plane.m_dist; // D3D uses negative distance

        // Check if plane changed
        if (m_userClipPlanesWorld[index].a != a || m_userClipPlanesWorld[index].b != b || m_userClipPlanesWorld[index].c != c ||
            m_userClipPlanesWorld[index].d != d) {
            m_userClipPlanesUpdated[index] = true;
            m_userClipPlanesWorld[index].a = a;
            m_userClipPlanesWorld[index].b = b;
            m_userClipPlanesWorld[index].c = c;
            m_userClipPlanesWorld[index].d = d;
        }

        ForceRecalcClipPlanes();
    };

    void CDevice::EnableClipPlane(int index, bool bEnable) {
        if (index < 0 || index >= 6)
            return;

        uint32_t planeBit     = 1 << index;
        bool currentlyEnabled = (m_userClipPlaneEnabled & planeBit) != 0;

        if (bEnable) {
            // Enable plane if not already enabled or if it was updated
            if (!currentlyEnabled || m_userClipPlanesUpdated[index]) {
                m_userClipPlaneEnabled |= planeBit;
                m_userClipPlanesUpdated[index] = false;
                setRenderState(D3DRS_CLIPPLANEENABLE, m_userClipPlaneEnabled);
            }
        } else {
            // Disable plane if currently enabled
            if (currentlyEnabled) {
                m_userClipPlaneEnabled &= ~planeBit;
                m_userClipPlanesUpdated[index] = false;
                setRenderState(D3DRS_CLIPPLANEENABLE, m_userClipPlaneEnabled);
            }
        }
    };

    void CDevice::ActuateClipPlanes(bool bForFFP) {
        uint32_t maxPlanes = GetMaxClipPlanes();

        for (uint32_t i = 0; i < maxPlanes; ++i) {
            if ((m_userClipPlaneEnabled & (1 << i)) != 0) {
                // For Fixed Function Pipeline, use world-space planes
                // For shaders, use projection-space planes
                const float* planeData = bForFFP ? &m_userClipPlanesWorld[i].a : &m_userClipPlanesProj[i].a;

                m_pd3dDevice->SetClipPlane(i, planeData);
            }
        }
    };

    int32_t CDevice::GetMaxAnisotropy() {
        return m_d3dCaps.MaxAnisotropy;
    };

    void CDevice::EnableFastClipPlane(bool bEnable) {
        m_fastClipEnabled = bEnable;
    };

    void CDevice::SetFastClipPlane(const hta::CPlane& plane) {
        float a = plane.m_normal.x;
        float b = plane.m_normal.y;
        float c = plane.m_normal.z;
        float d = -plane.m_dist;

        if (m_fastClipPlane.a == a && m_fastClipPlane.b == b && m_fastClipPlane.c == c && m_fastClipPlane.d == d) {
            return;
        }

        m_fastClipPlane.a = a;
        m_fastClipPlane.b = b;
        m_fastClipPlane.c = c;
        m_fastClipPlane.d = d;

        const hta::CMatrix& proj              = MatGetProj();
        const hta::CMatrix& view              = GetViewMatrix();
        hta::CMatrix worldToProjInvTransposed = (view * proj).PlaneTransform();

        hta::CVector4 planeVec(a, b, c, d);
        hta::CVector4 projPlane = worldToProjInvTransposed * planeVec;

        m_fastClipPlaneProjMatrix.Identity();
        m_fastClipPlaneProjMatrix(0, 2) = projPlane.x;
        m_fastClipPlaneProjMatrix(1, 2) = projPlane.y;
        m_fastClipPlaneProjMatrix(2, 2) = projPlane.z;
        m_fastClipPlaneProjMatrix(3, 2) = projPlane.w;

        m_fastClipPlaneProjMatrix = m_fastClipPlaneProjMatrix * proj;
    }

    void CDevice::ResetStats() {
        memset(&m_stats, 0, sizeof(m_stats));

        // Reset per-effect statistics
        for (auto& pair : m_effects) {
            EffectImpl* effect      = pair.second;
            effect->m_numPrimitives = 0;
            effect->m_numDIPs       = 0;
        }
    };

    void CDevice::GetStats(RenderStats& stats) const {
        stats = m_stats;
    };

    void CDevice::GetDeviceMemStats(DeviceMemStats& stats) {
        if (!m_isDevMemStatsValid) {
            UpdateVBMemStats();
            UpdateIBMemStats();
            UpdateTexMemStats();
            m_isDevMemStatsValid = true;
        }

        stats = m_devMemStats;
    };

    void UnifyFileName0(hta::CStr* fileName) {
        if (!fileName || !fileName->m_charPtr)
            return;

        char* str  = fileName->m_charPtr;
        size_t len = strlen(str);

        // Convert backslashes to forward slashes
        for (size_t i = 0; i < len; ++i) {
            if (str[i] == '\\')
                str[i] = '/';
        }

        // Convert to lowercase
        if (len > 0) {
            LCMapStringA(LOCALE_USER_DEFAULT, LCMAP_LOWERCASE, str, len + 1, str, len + 1);
        }
    }

    hta::CStr NameFromFileName(const hta::CStr& source) {
        hta::CStr path = source;
        UnifyFileName0(&path);

        if (!path.m_charPtr)
            return hta::CStr(source);

        // Find last forward slash
        const char* lastSlash = strrchr(path.m_charPtr, '/');

        if (lastSlash) {
            int slashPos = lastSlash - path.m_charPtr;
            if (slashPos != -1) {
                int len = path.m_charPtr ? strlen(path.m_charPtr) : 0;
                return path.substr(slashPos + 1, len);
            }
        }

        return hta::CStr(source);
    };

    void CDevice::ShowStats() const {
        if (!m_pSysFont)
            return;

        int y = 20;
        RECT rect;

        // Draw headers
        SetRect(&rect, 2, 2, 0, 0);
        m_pSysFont->DrawTextA(nullptr, "Effect file", -1, &rect, DT_NOCLIP, 0xFF000000);

        SetRect(&rect, 200, 2, 0, 0);
        m_pSysFont->DrawTextA(nullptr, "#prims", -1, &rect, DT_NOCLIP, 0xFF000000);

        SetRect(&rect, 300, 2, 0, 0);
        m_pSysFont->DrawTextA(nullptr, "#dips", -1, &rect, DT_NOCLIP, 0xFF000000);

        // Draw per-effect statistics
        uint32_t totalPrimitives = 0;
        uint32_t totalDIPs       = 0;

        for (const auto& pair : m_effects) {
            EffectImpl* effect = pair.second;

            if (!effect || effect->m_numPrimitives == 0)
                continue;

            // Effect name
            SetRect(&rect, 2, y, 0, 0);
            hta::CStr effectName = NameFromFileName(effect->mFileName);
            m_pSysFont->DrawTextA(nullptr, effectName.c_str(), -1, &rect, DT_NOCLIP, 0xFFFFFFFF);

            // Primitives count
            SetRect(&rect, 200, y, 0, 0);
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%u", effect->m_numPrimitives);
            m_pSysFont->DrawTextA(nullptr, buffer, -1, &rect, DT_NOCLIP, 0xFFFFFFFF);

            // DIPs count
            SetRect(&rect, 300, y, 0, 0);
            snprintf(buffer, sizeof(buffer), "%u", effect->m_numDIPs);
            m_pSysFont->DrawTextA(nullptr, buffer, -1, &rect, DT_NOCLIP, 0xFFFFFFFF);

            totalPrimitives += effect->m_numPrimitives;
            totalDIPs += effect->m_numDIPs;

            y += 18;
        }

        // Draw totals
        y += 18;

        SetRect(&rect, 2, y, 0, 0);
        m_pSysFont->DrawTextA(nullptr, "Total", -1, &rect, DT_NOCLIP, 0xFFFFFFFF);

        SetRect(&rect, 200, y, 0, 0);
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%u", totalPrimitives);
        m_pSysFont->DrawTextA(nullptr, buffer, -1, &rect, DT_NOCLIP, 0xFFFFFFFF);

        SetRect(&rect, 300, y, 0, 0);
        snprintf(buffer, sizeof(buffer), "%u", totalDIPs);
        m_pSysFont->DrawTextA(nullptr, buffer, -1, &rect, DT_NOCLIP, 0xFFFFFFFF);
    };

    MeshHandle
    CDevice::AddMesh(VertexType type, void* verts, int numVerts, uint16_t* tris, int numTris, SubmeshInfo* subs, int numSubs, int* remap) {
        MeshHandle handle;
        handle.m_handle = -1;

        // Get vertex format info
        uint32_t fvf;
        int vertSize;
        IDirect3DVertexDeclaration9* vdecl;
        GetVertexInfo(type, fvf, vertSize, vdecl);

        // Validate indices
        for (int i = 0; i < numTris; ++i) {
            uint16_t i0 = tris[i * 3 + 0];
            uint16_t i1 = tris[i * 3 + 1];
            uint16_t i2 = tris[i * 3 + 2];

            if (i0 > numVerts || i1 > numVerts || i2 > numVerts) {
                LOG_ERROR("Incoming mesh has index out of bounds in tri #%d: (%d,%d,%d), max vertex index is %d", i, i0, i1, i2, numVerts);
                return handle;
            }
        }

        // Find free slot or add new one
        int32_t meshIndex = 0;
        for (; meshIndex < m_meshes.size(); ++meshIndex) {
            if (!m_meshes[meshIndex].m_mesh)
                break;
        }

        if (meshIndex >= m_meshes.size()) {
            m_meshes.push_back(CMesh());
        }

        CMesh& mesh = m_meshes[meshIndex];

        // Store mesh metadata
        mesh.m_numVerts   = numVerts;
        mesh.m_numTris    = numTris;
        mesh.m_vertexType = type;

        // Allocate and copy index data
        mesh.m_indices = new uint16_t[numTris * 3];
        memcpy(mesh.m_indices, tris, numTris * 6);

        // Allocate and copy vertex data
        mesh.m_verts = new uint8_t[numVerts * vertSize];
        memcpy(mesh.m_verts, verts, numVerts * vertSize);

        // Create D3DX mesh
        HRESULT hr = D3DXCreateMeshFVF(numTris, numVerts, D3DXMESH_MANAGED | D3DXMESH_WRITEONLY, fvf, m_pd3dDevice, &mesh.m_mesh);

        MeshHandle object{meshIndex};

        if (FAILED(hr)) {
            LOG_ERROR("Cannot create mesh: %s", getD3dErrorStr(hr));
            ReleaseMesh(handle);
            return handle;
        }

        // Fill index buffer
        void* ibData;
        hr = mesh.m_mesh->LockIndexBuffer(D3DLOCK_DISCARD, &ibData);
        if (FAILED(hr)) {
            LOG_ERROR("Cannot lock index buffer for mesh: %s", getD3dErrorStr(hr));
            ReleaseMesh(object);
            return handle;
        }

        memcpy(ibData, mesh.m_indices, numTris * 6);
        mesh.m_mesh->UnlockIndexBuffer();

        // Fill vertex buffer
        void* vbData;
        hr = mesh.m_mesh->LockVertexBuffer(D3DLOCK_DISCARD, &vbData);
        if (FAILED(hr)) {
            LOG_ERROR("Cannot lock vertex buffer for mesh: %s", getD3dErrorStr(hr));
            ReleaseMesh(object);
            return handle;
        }

        memcpy(vbData, mesh.m_verts, numVerts * vertSize);
        mesh.m_mesh->UnlockVertexBuffer();

        // Generate adjacency - use RAII for temporary buffer
        std::vector<DWORD> adjacency(numTris * 3);
        hr = mesh.m_mesh->GenerateAdjacency(0.0f, adjacency.data());
        if (FAILED(hr)) {
            LOG_ERROR("Cannot generate adjacency: %s", getD3dErrorStr(hr));
            ReleaseMesh(object);
            return handle;
        }

        // Optimize mesh
        ID3DXBuffer* remapBuffer = nullptr;
        hr                       = mesh.m_mesh->OptimizeInplace(
            D3DXMESHOPT_COMPACT | D3DXMESHOPT_ATTRSORT | D3DXMESHOPT_VERTEXCACHE, adjacency.data(), adjacency.data(), nullptr, &remapBuffer
        );

        if (FAILED(hr)) {
            LOG_ERROR("Could not optimize mesh: %s", getD3dErrorStr(hr));
            if (remapBuffer) {
                remapBuffer->Release();
            }
            ReleaseMesh(object);
            return handle;
        }

        // Copy remap data if requested
        if (remapBuffer) {
            if (remap) {
                memcpy(remap, remapBuffer->GetBufferPointer(), remapBuffer->GetBufferSize());
            }
            remapBuffer->Release();
        }

        handle.m_handle = meshIndex;
        return handle;
    };

    int CDevice::ReleaseMesh(MeshHandle& handle) {
        if (handle.m_handle < 0)
            return 0;

        if (handle.m_handle >= m_meshes.size())
            return 0;

        CMesh& mesh = m_meshes[handle.m_handle];

        if (!mesh.m_mesh)
            return 0;

        // Decrement reference count
        --mesh.mRefs;

        // Only actually release when refcount reaches zero
        if (mesh.mRefs <= 0) {
            // Release D3DX resources
            if (mesh.m_mesh) {
                mesh.m_mesh->Release();
                mesh.m_mesh = nullptr;
            }

            if (mesh.m_pmesh) {
                mesh.m_pmesh->Release();
                mesh.m_pmesh = nullptr;
            }

            // Delete CPU-side copies
            if (mesh.m_verts) {
                delete[] static_cast<uint8_t*>(mesh.m_verts);
                mesh.m_verts = nullptr;
            }

            if (mesh.m_indices) {
                delete[] mesh.m_indices;
                mesh.m_indices = nullptr;
            }

            // Invalidate handle
            handle.m_handle = -1;
        }

        return mesh.mRefs;
    };

    void CDevice::RenderMesh(const MeshHandle& handle, float lod) {
        if (handle.m_handle < 0)
            return;

        if (handle.m_handle >= m_meshes.size())
            return;

        CMesh& mesh = m_meshes[handle.m_handle];

        if (!mesh.m_mesh)
            return;

        // Apply render states
        ActuateStates(true);

        // Draw the mesh
        mesh.m_mesh->DrawSubset(0);

        // Update statistics
        m_stats.polyCount += mesh.m_numTris;
    };

    void CDevice::StartRenderMeshes() {};

    void CDevice::FinishRenderMeshes() {};

    int32_t CDevice::OptimizeGeometryToSingleStrip(
        VertexType vt,
        void* vertsIn,
        int32_t numVertsIn,
        uint16_t* indicesIn,
        int32_t numTrisIn,
        int32_t zeroBaseIndex,
        void** vertsOut,
        int32_t* numVertsOut,
        int32_t** remap,
        uint16_t** singleStripOut,
        int32_t* numSingleStripOutIndices
    ) {
        // Initialize outputs
        *singleStripOut           = nullptr;
        *numSingleStripOutIndices = 0;
        *vertsOut                 = nullptr;
        *numVertsOut              = 0;

        // Allocate remap buffer
        *remap = new int32_t[numVertsIn];

        // Create temporary mesh for optimization
        MeshHandle meshHandle = AddMesh(vt, vertsIn, numVertsIn, indicesIn, numTrisIn, nullptr, 0, *remap);

        if (meshHandle.m_handle < 0) {
            delete[] *remap;
            *remap = nullptr;
            return 0;
        }

        CMesh& mesh = m_meshes[meshHandle.m_handle];

        // Convert mesh to single triangle strip
        ID3DXMesh* stripMesh = nullptr;
        uint32_t numIndices  = 0;

        HRESULT hr = D3DXConvertMeshSubsetToSingleStrip(
            mesh.m_mesh, 0, D3DXMESH_MANAGED | D3DXMESH_WRITEONLY, (LPDIRECT3DINDEXBUFFER9*)&stripMesh, (DWORD*)&numIndices
        );

        if (FAILED(hr)) {
            LOG_ERROR("Cannot convert mesh to single strip");
            ReleaseMesh(meshHandle);
            return 0;
        }

        // Extract strip indices
        void* indexData;
        hr = stripMesh->LockIndexBuffer(D3DLOCK_READONLY, &indexData);
        if (SUCCEEDED(hr)) {
            *numSingleStripOutIndices = numIndices;
            *singleStripOut           = new uint16_t[numIndices];
            memcpy(*singleStripOut, indexData, numIndices * sizeof(uint16_t));
            stripMesh->UnlockIndexBuffer();
        }

        stripMesh->Release();

        if (FAILED(hr)) {
            LOG_ERROR("Cannot lock index buffer for strip mesh");
            ReleaseMesh(meshHandle);
            return 0;
        }

        // Get vertex buffer info
        IDirect3DVertexBuffer9* vb = nullptr;
        mesh.m_mesh->GetVertexBuffer(&vb);

        D3DVERTEXBUFFER_DESC vbDesc;
        vb->GetDesc(&vbDesc);
        vb->Release();

        // Extract optimized vertices
        void* vbData;
        hr = mesh.m_mesh->LockVertexBuffer(D3DLOCK_READONLY, &vbData);
        if (FAILED(hr)) {
            LOG_ERROR("Cannot lock vertex buffer for mesh: %s", getD3dErrorStr(hr));
            delete[] *singleStripOut;
            *singleStripOut = nullptr;
            ReleaseMesh(meshHandle);
            return 0;
        }

        // Calculate vertex count and copy data
        uint32_t fvf;
        int vertSize;
        IDirect3DVertexDeclaration9* vdecl;
        GetVertexInfo(vt, fvf, vertSize, vdecl);

        *numVertsOut = vbDesc.Size / vertSize;
        *vertsOut    = new uint8_t[vbDesc.Size];
        memcpy(*vertsOut, vbData, vbDesc.Size);

        mesh.m_mesh->UnlockVertexBuffer();

        // Cleanup
        ReleaseMesh(meshHandle);

        return 1; // Success
    };
    int32_t CDevice::OptimizeGeometryToTriList(
        VertexType vt,
        void* vertsIn,
        int32_t numVertsIn,
        uint16_t* indicesIn,
        int32_t numTrisIn,
        int32_t zeroBaseIndex,
        void** vertsOut,
        int32_t* numVertsOut,
        int32_t** remap,
        uint16_t** trisIndices,
        int32_t* numTrisIndices
    ) {
        // Initialize outputs
        *trisIndices    = nullptr;
        *numTrisIndices = 0;
        *vertsOut       = nullptr;
        *numVertsOut    = 0;

        // Allocate remap buffer
        *remap = new int32_t[numVertsIn];

        // Create temporary mesh for optimization
        MeshHandle meshHandle = AddMesh(vt, vertsIn, numVertsIn, indicesIn, numTrisIn, nullptr, 0, *remap);

        if (meshHandle.m_handle < 0) {
            delete[] *remap;
            *remap = nullptr;
            return 0;
        }

        CMesh& mesh = m_meshes[meshHandle.m_handle];

        // Extract optimized index buffer (triangle list)
        void* indexData;
        HRESULT hr = mesh.m_mesh->LockIndexBuffer(D3DLOCK_READONLY, &indexData);
        if (SUCCEEDED(hr)) {
            uint32_t numFaces   = mesh.m_mesh->GetNumFaces();
            uint32_t numIndices = numFaces * 3;

            *numTrisIndices = numIndices;
            *trisIndices    = new uint16_t[numIndices];
            memcpy(*trisIndices, indexData, numIndices * sizeof(uint16_t));

            mesh.m_mesh->UnlockIndexBuffer();
        }

        // Get vertex buffer info
        IDirect3DVertexBuffer9* vb = nullptr;
        mesh.m_mesh->GetVertexBuffer(&vb);

        D3DVERTEXBUFFER_DESC vbDesc;
        vb->GetDesc(&vbDesc);
        vb->Release();

        // Extract optimized vertices
        void* vbData;
        hr = mesh.m_mesh->LockVertexBuffer(D3DLOCK_READONLY, &vbData);
        if (FAILED(hr)) {
            LOG_ERROR("Cannot lock vertex buffer for mesh: %s", getD3dErrorStr(hr));
            delete[] *trisIndices;
            *trisIndices = nullptr;
            ReleaseMesh(meshHandle);
            return 0;
        }

        // Calculate vertex count and copy data
        uint32_t fvf;
        int vertSize;
        IDirect3DVertexDeclaration9* vdecl;
        GetVertexInfo(vt, fvf, vertSize, vdecl);

        *numVertsOut = vbDesc.Size / vertSize;
        *vertsOut    = new uint8_t[vbDesc.Size];
        memcpy(*vertsOut, vbData, vbDesc.Size);

        mesh.m_mesh->UnlockVertexBuffer();

        // Cleanup
        ReleaseMesh(meshHandle);

        return 1; // Success
    };

    void* CDevice::GetInternalData() {
        return m_pd3dDevice;
    };

    void CDevice::SetVsFloatConst(unsigned int RegisterIndex, const float* pConstantData, unsigned int RegisterCount) {
        this->mVSUniforms.Set(RegisterIndex, pConstantData, RegisterCount);
    };

    void CDevice::SetVsIntConst(unsigned int RegisterIndex, const int* pConstantData, unsigned int RegisterCount) {
        this->mVSUniforms.Set(RegisterIndex, pConstantData, RegisterCount);
    };

    void CDevice::SetVsBoolConst(unsigned int RegisterIndex, const int* pConstantData, unsigned int RegisterCount) {
        this->mVSUniforms.Set(RegisterIndex, pConstantData, RegisterCount);
    };

    void CDevice::SetPsFloatConst(unsigned int RegisterIndex, const float* pConstantData, unsigned int RegisterCount) {
        this->mFSUniforms.Set(RegisterIndex, pConstantData, RegisterCount);
    };

    void CDevice::SetPsIntConst(unsigned int RegisterIndex, const int* pConstantData, unsigned int RegisterCount) {
        this->mFSUniforms.Set(RegisterIndex, pConstantData, RegisterCount);
    };

    void CDevice::SetPsBoolConst(unsigned int RegisterIndex, const int* pConstantData, unsigned int RegisterCount) {
        this->mFSUniforms.Set(RegisterIndex, pConstantData, RegisterCount);
    };

    void UnifyFileName(hta::CStr* fileName) {
        char* m_charPtr = fileName->m_charPtr;
        if (!m_charPtr)
            return;

        size_t len = strlen(m_charPtr);

        // Replace forward slashes with backslashes
        for (size_t i = 0; i < len; ++i) {
            if (m_charPtr[i] == '/')
                m_charPtr[i] = '\\';
        }

        // Convert to lowercase
        if (len > 0) {
            LCMapStringA(LOCALE_USER_DEFAULT, LCMAP_LOWERCASE, m_charPtr, len + 1, m_charPtr, len + 1);
        }
    }

    IAsmShader* CDevice::NewAsmShader(const char* fileName, IAsmShader::Type type) {
        hta::CStr fName(fileName);
        UnifyFileName(&fName);

        // Look up shader in cache
        auto it = m_AsmShaders.lower_bound(fName);

        // If found in cache, return existing shader with increased ref count
        if (it != m_AsmShaders.end() && !(fName < it->first)) {
            AsmShaderImpl* shader = it->second;
            shader->AddRef();
            return shader;
        }

        // Create new shader
        AsmShaderImpl* shader = new AsmShaderImpl();

        // Load shader from file
        if (!shader->LoadFromFile(fileName, type)) {
            delete shader;
            return nullptr;
        }

        // Add to cache and return
        shader->AddRef();
        m_AsmShaders[fName] = shader;

        return shader;
    }

    IHlslShader* CDevice::NewHlslShader(
        const char* fileName, const char* entryFunc, IHlslShader::Profile profile, const std::vector<CompileParam>& compileParams
    ) {
        // Create shader identifier
        ShaderIdData shaderId;
        shaderId.filename = hta::CStr(fileName);
        UnifyFileName(&shaderId.filename);
        shaderId.compileParams = compileParams;

        // Sort and remove duplicates from compile params
        std::sort(shaderId.compileParams.begin(), shaderId.compileParams.end());
        shaderId.compileParams.erase(
            std::unique(shaderId.compileParams.begin(), shaderId.compileParams.end()), shaderId.compileParams.end()
        );

        // Look up shader in cache
        auto it = m_HlslShaders.lower_bound(shaderId);

        // If found in cache, return existing shader with increased ref count
        if (it != m_HlslShaders.end() && !(shaderId < it->first)) {
            HlslShaderImpl* shader = it->second;
            shader->AddRef();
            return shader;
        }

        // Create new shader
        HlslShaderImpl* shader = new HlslShaderImpl();

        // Load shader from file
        if (!shader->LoadFromFile(fileName, entryFunc, profile, shaderId.compileParams)) {
            delete shader;
            return nullptr;
        }

        // Add to cache and return
        shader->AddRef();
        m_HlslShaders[shaderId] = shader;

        return shader;
    }

    IHlslShader* CDevice::NewHlslShader(const char* filename, const char* entry, IHlslShader::Profile profile) {
        std::vector<CompileParam> compileParams{};
        return NewHlslShader(filename, entry, profile, compileParams);
    };

    IEffect* CDevice::NewEffect(const char* fileName, bool bApplyGlobalParams, const std::vector<CompileParam>& compileParams) {
        // Create effect identifier
        ShaderIdData fxId;
        fxId.filename = hta::CStr(fileName);
        UnifyFileName(&fxId.filename);
        fxId.compileParams = compileParams;

        // Sort and remove duplicates from compile params
        std::sort(fxId.compileParams.begin(), fxId.compileParams.end());
        fxId.compileParams.erase(std::unique(fxId.compileParams.begin(), fxId.compileParams.end()), fxId.compileParams.end());

        // Look up effect in cache
        auto it = m_effects.find(fxId);

        // If found in cache, return existing effect with increased ref count
        if (it != m_effects.end()) {
            EffectImpl* effect = it->second;
            effect->AddRef();
            return effect;
        }

        // Create new effect
        EffectImpl* effect = new EffectImpl(bApplyGlobalParams);

        // Load effect from file
        if (!effect->LoadFromFile(fxId.filename.m_charPtr, fxId.compileParams)) {
            delete effect;
            return nullptr;
        }

        // Add to cache and return
        effect->AddRef();
        m_effects[fxId] = effect;

        return effect;
    }

    IEffect* CDevice::NewEffect(const char* fileName, bool bApplyGlobalParams) {
        return NewEffect(fileName, bApplyGlobalParams, {});
    };

    static const char* StringTable[] = {
        "WORLD_MATRIX",       // 0
        "VIEW_MATRIX",        // 1
        "PROJECTION_MATRIX",  // 2
        "MODEL_VIEW_MATRIX",  // 3
        "INV_WORLD_MATRIX",   // 4
        "TOTAL_MATRIX",       // 5
        "VIEW_POS",           // 6
        "DIFFUSE_MAP_0",      // 7
        "CUBE_MAP_0",         // 8
        "BUMP_MAP_0",         // 9
        "DETAIL_MAP_0",       // 10
        "LIGHT_MAP_0",        // 11
        "NORMAL_CUBE_MAP",    // 12
        "TIME_LINEAR",        // 13
        "TREE_BEND_TERM",     // 14
        "LIGHT_AMBIENT",      // 15
        "LIGHT_DIFFUSE",      // 16
        "LIGHT_PLANT",        // 17
        "LIGHT_SPECULAR",     // 18
        "FOG_TERM",           // 19
        "TRANSPARENCY",       // 20
        "TRANS_START_DIST",   // 21
        "TRANS_OBJECT_WIDTH", // 22
        "TMP_LIGHT0_DIR",     // 23 ✅
        "USER_FLOAT_PARAM",   // 24
        "USER_FLOAT_PARAM2",  // 25
        "USER_FLOAT_PARAM3",  // 26
        "USER_FLOAT3_PARAM",  // 27
        "USER_FLOAT3_PARAM2", // 28
        "USER_FLOAT4_PARAM",  // 29
        "USER_FLOAT4x4_PARAM" // 30
    };

    const char* CDevice::EffectParameterToString(IEffect::Parameter p) {
        return StringTable[p];
    }

    IEffect::Parameter CDevice::StringToEffectParameter(const char* str) {
        for (int i = 0; i < 49; ++i) {
            if (strcmp(str, StringTable[i]) == 0)
                return IEffect::Parameter(i);
        }
        return IEffect::Parameter(50);
    };

    const char* CompileParamsNames[2]{
        "COMPILE_INSTANCING_VERSION",
        "COMPILE_SUPPORT_SHADOWMAP",
    };

    const char* CDevice::CompileParamToString(CompileParam p) {
        return CompileParamsNames[p];
    };

    CompileParam CDevice::StringToCompileParam(const char* str) {
        for (int i = 0; i < 2; ++i) {
            if (strcmp(str, CompileParamsNames[i]) == 0)
                return CompileParam(i);
        }
        return CompileParam(2);
    };

    bool CDevice::ReloadShaders() {
        bool res = true;

        // Create temporary ASM shader for testing reloads
        AsmShaderImpl* tempAsmShader = new AsmShaderImpl();

        // Reload all ASM shaders
        for (auto& pair : m_AsmShaders) {
            AsmShaderImpl* shader = pair.second;
            IAsmShader::Type type = shader->m_type;

            // Try loading with temp shader first
            if (tempAsmShader->LoadFromFile(shader->mFileName.m_charPtr, type)) {
                // Reload succeeded, invalidate both and reload actual shader
                tempAsmShader->Invalidate();
                shader->Invalidate();

                if (!shader->LoadFromFile(shader->mFileName.m_charPtr, type)) {
                    LOG_ERROR("Failed to reload ASM shader");
                }
            } else {
                // Reload failed
                tempAsmShader->Invalidate();
                res = false;
            }
        }

        // Clean up temp ASM shader
        tempAsmShader->LoadFromFile("no file", static_cast<IAsmShader::Type>(0));
        delete tempAsmShader;

        // Create temporary HLSL shader for testing reloads
        HlslShaderImpl* tempHlslShader = new HlslShaderImpl();

        // Reload all HLSL shaders
        for (auto& pair : m_HlslShaders) {
            HlslShaderImpl* shader       = pair.second;
            IHlslShader::Profile profile = shader->m_profile;

            // Try loading with temp shader first
            if (tempHlslShader->LoadFromFile(
                    shader->mFileName.m_charPtr, shader->m_entryPoint.m_charPtr, profile, shader->m_compileParams
                )) {
                // Reload succeeded, invalidate both and reload actual shader
                tempHlslShader->Invalidate();
                shader->Invalidate();

                if (!shader->LoadFromFile(shader->mFileName.m_charPtr, shader->m_entryPoint.m_charPtr, profile, shader->m_compileParams)) {
                    LOG_ERROR("Failed to reload HLSL shader");
                }
            } else {
                // Reload failed
                tempHlslShader->Invalidate();
            }
        }

        // Clean up temp HLSL shader
        std::vector<CompileParam> emptyParams;
        tempHlslShader->LoadFromFile("no file", "with this name", IHlslShader::PS_2_0, emptyParams);
        delete tempHlslShader;

        // Create temporary effect for testing reloads
        EffectImpl* tempEffect = new EffectImpl(true);

        // Reload all effects
        for (auto& pair : m_effects) {
            EffectImpl* effect   = pair.second;
            unsigned int curTech = effect->GetCurTechnique();

            // Try loading with temp effect first
            if (tempEffect->LoadFromFile(effect->mFileName.m_charPtr, effect->m_compileParams)) {
                // Reload succeeded, invalidate both and reload actual effect
                tempEffect->Invalidate();
                effect->Invalidate();

                if (!effect->LoadFromFile(effect->mFileName.m_charPtr, effect->m_compileParams)) {
                    LOG_ERROR("Failed to reload effect");
                }
            } else {
                // Reload failed
                tempEffect->Invalidate();
            }

            // Restore technique
            effect->SetCurTechnique(curTech);
        }

        // Clean up temp effect
        tempEffect->LoadFromFile("no file", emptyParams);
        delete tempEffect;

        if (res) {
            LOG_INFO("Reloaded shaders ok");
        }

        return res;
    };

    void CDevice::AddChangeShaderMacro(const ShaderMacro& macro) {
        // Find existing macro with same name
        auto it = std::find_if(m_shadersMacros.begin(), m_shadersMacros.end(), [&](const MacroData& data) {
            return data.macro.name == macro.name;
        });

        // If not found, add new macro
        if (it == m_shadersMacros.end()) {
            addMacro(macro.name, macro.definition, true);
            createD3DXMacros();
            saveShadersMacros();
            return;
        }

        // If found and user-defined, update definition if changed
        if (it->userDefined && it->macro.definition != macro.definition) {
            it->macro.definition = macro.definition;
            createD3DXMacros();
            saveShadersMacros();
        }
    };

    void CDevice::DeleteShaderMacro(const hta::CStr& macroName) {
        // Find existing macro with same name
        auto it = std::find_if(m_shadersMacros.begin(), m_shadersMacros.end(), [&](const MacroData& data) {
            return data.macro.name == macroName;
        });

        // If found and user-defined, remove it
        if (it != m_shadersMacros.end() && it->userDefined) {
            m_shadersMacros.erase(it);
            createD3DXMacros();
            saveShadersMacros();
        }
    };

    int32_t CDevice::DrawIndexedPrimitiveEffect(
        PrimType Type, IEffect* effect, uint32_t MinIndex, uint32_t NumVertices, uint32_t StartIndex, uint32_t PrimitiveCount
    ) {
        EffectImpl* effectImpl = static_cast<EffectImpl*>(effect);

        // Mark effect draw
        wchar_t marker[256];

        // If effect is invalid, fall back to regular drawing
        if (!effectImpl || !effectImpl->IsValid()) {
            D3DPERF_SetMarker(D3DCOLOR_XRGB(255, 0, 0), L"INVALID EFFECT - Using FFP fallback");
            return DrawIndexedPrimitive(Type, MinIndex, NumVertices, StartIndex, PrimitiveCount);
        }

        // Get effect info for marker
        const char* fileName      = effectImpl->mFileName.m_charPtr;
        const char* techniqueName = effectImpl->GetCurTechniqueName();
        swprintf(
            marker,
            256,
            L"Effect: %S | Tech: %S | Prims: %d",
            fileName ? fileName : "Unknown",
            techniqueName ? techniqueName : "Default",
            PrimitiveCount
        );
        D3DPERF_BeginEvent(D3DCOLOR_XRGB(255, 128, 0), marker);

        // Check if render to null is enabled
        const hta::m3d::CVar& renderToNull = hta::m3d::Kernel::Instance()->GetEngineCfg().m_r_renderToNull;
        bool isRenderToNull                = (renderToNull.m_type == hta::m3d::CVar::CVAR_BOOL) ? renderToNull.m_b : (renderToNull.m_i > 0);

        if (isRenderToNull) {
            D3DPERF_SetMarker(D3DCOLOR_XRGB(128, 128, 128), L"RenderToNull enabled - skipping");
            D3DPERF_EndEvent();
            return 1;
        }

        // Actuate states before rendering
        ActuateStates(false);

        // Begin effect and render all passes
        int numPasses = effectImpl->Begin();

        swprintf(marker, 256, L"Rendering %d pass(es)", numPasses);
        D3DPERF_SetMarker(D3DCOLOR_XRGB(0, 255, 255), marker);

        assert(numPasses > 0 && "Empty draw passes!");

        for (int pass = 0; pass < numPasses; ++pass) {
            swprintf(marker, 256, L"Pass %d/%d (Verts=%d, Prims=%d)", pass + 1, numPasses, NumVertices, PrimitiveCount);
            // D3DPERF_BeginEvent(D3DCOLOR_XRGB(128, 255, 128), marker);

            effectImpl->BeginPass(pass);

            // Update index buffer base
            m_curIbBaseIdx = m_latchedIbBaseIdx;

            // Draw indexed primitive
            this->mVSUniforms.Commit();
            this->mFSUniforms.Commit();
            _SetLastResult(m_pd3dDevice->DrawIndexedPrimitive(
                m3dPtToD3dPt[Type], m_latchedIbBaseIdx, MinIndex, NumVertices, StartIndex, PrimitiveCount
            ));

            // Update stats
            m_stats.polyCount += PrimitiveCount;
            effectImpl->m_numPrimitives += PrimitiveCount;

            effectImpl->EndPass();

            D3DPERF_EndEvent(); // End pass marker
        }

        effectImpl->End();

        // Update DIP stats
        m_stats.DIPs += numPasses;
        effectImpl->m_numDIPs += numPasses;

        D3DPERF_EndEvent(); // End effect marker

        return 1;
    }

    int32_t CDevice::DrawPrimitiveEffect(PrimType PrimitiveType, IEffect* effect, uint32_t StartVertex, uint32_t PrimitiveCount) {
        EffectImpl* effectImpl = static_cast<EffectImpl*>(effect);

        // If effect is invalid, fall back to regular drawing
        if (!effectImpl || !effectImpl->IsValid()) {
            return DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
        }

        // Check if render to null is enabled
        const hta::m3d::CVar& renderToNull = hta::m3d::Kernel::Instance()->GetEngineCfg().m_r_renderToNull;
        bool isRenderToNull                = (renderToNull.m_type == hta::m3d::CVar::CVAR_BOOL) ? renderToNull.m_b : (renderToNull.m_i > 0);

        if (isRenderToNull)
            return 1;

        // Actuate states before rendering
        ActuateStates(false);

        // Begin effect and render all passes
        int numPasses = effectImpl->Begin();

        assert(numPasses > 0 && "Empty draw passes!");
        for (int pass = 0; pass < numPasses; ++pass) {
            effectImpl->BeginPass(pass);

            // Draw primitive
            this->mVSUniforms.Commit();
            this->mFSUniforms.Commit();
            _SetLastResult(m_pd3dDevice->DrawPrimitive(m3dPtToD3dPt[PrimitiveType], StartVertex, PrimitiveCount));

            // Update stats
            m_stats.polyCount += PrimitiveCount;
            effectImpl->m_numPrimitives += PrimitiveCount;

            effectImpl->EndPass();
        }

        effectImpl->End();

        // Update DIP stats
        m_stats.DIPs += numPasses;
        effectImpl->m_numDIPs += numPasses;

        return 1;
    };

    BOOL CDevice::DrawIndexedPrimitiveShader(
        PrimType Type, unsigned int MinIndex, unsigned int NumVertices, unsigned int StartIndex, unsigned int PrimitiveCount
    ) {
        ActuateStates(false);

        // Update index buffer base
        m_curIbBaseIdx = m_latchedIbBaseIdx;

        // Draw indexed primitive
        this->mVSUniforms.Commit();
        this->mFSUniforms.Commit();
        HRESULT hr =
            m_pd3dDevice->DrawIndexedPrimitive(m3dPtToD3dPt[Type], m_latchedIbBaseIdx, MinIndex, NumVertices, StartIndex, PrimitiveCount);

        // Update stats
        m_stats.polyCount += PrimitiveCount;
        ++m_stats.DIPs;

        _SetLastResult(hr);
        return SUCCEEDED(hr);
    };

    int32_t CDevice::DrawPrimitiveShader(PrimType, uint32_t, uint32_t) {
        assert(false && "Not implemented draw call");
        return 0;
    };

    IQuery* CDevice::NewQuery(IQuery::Type type) {
        Query* query = new Query(type);

        m_queries.push_back(query);
        query->AddRef();

        bool isSupported = false;

        switch (type) {
        case IQuery::QUERY_VCACHE:
            isSupported = m_featureSupported[13];
            break;
        case IQuery::QUERY_EVENT:
            isSupported = m_featureSupported[14];
            break;
        case IQuery::QUERY_OCCLUSION:
            isSupported = m_featureSupported[15];
            break;
        case IQuery::QUERY_TIMESTAMP:
            isSupported = m_featureSupported[16];
            break;
        case IQuery::QUERY_TIMESTAMPDISJOINT:
            isSupported = m_featureSupported[17];
            break;
        case IQuery::QUERY_TIMESTAMPFREQ:
            isSupported = m_featureSupported[18];
            break;
        case IQuery::QUERY_PIPELINETIMINGS:
            isSupported = m_featureSupported[19];
            break;
        case IQuery::QUERY_INTERFACETIMINGS:
            isSupported = m_featureSupported[20];
            break;
        case IQuery::QUERY_VERTEXTIMINGS:
            isSupported = m_featureSupported[21];
            break;
        case IQuery::QUERY_BANDWIDTHTIMINGS:
            isSupported = m_featureSupported[23];
            break;
        case IQuery::QUERY_CACHEUTILIZATION:
            isSupported = m_featureSupported[24];
            break;
        default:
            isSupported = false;
            break;
        }

        if (isSupported) {
            query->m_state = IQuery::QUERY_SIGNALED;

            if (FAILED(m_pd3dDevice->CreateQuery(static_cast<D3DQUERYTYPE>(type), &query->m_query))) {
                LOG_ERROR("Failed to create D3D9 query");
            }
        } else {
            query->m_state = IQuery::QUERY_NOT_SUPPORT;
        }

        return query;
    };

    void CDevice::OnEffectDestructor(EffectImpl* effect) {
        // Create shader identifier from effect
        ShaderIdData fxId;
        fxId.filename      = effect->mFileName;
        fxId.compileParams = effect->m_compileParams;
        UnifyFileName(&fxId.filename);

        // Remove all matching effects from cache
        auto range = m_effects.equal_range(fxId);
        m_effects.erase(range.first, range.second);
    };

    void CDevice::OnHlslShaderDestructor(HlslShaderImpl* shader) {
        // Create shader identifier from shader
        ShaderIdData shaderId;
        shaderId.filename      = shader->mFileName;
        shaderId.compileParams = shader->m_compileParams;
        UnifyFileName(&shaderId.filename);

        // Remove all matching shaders from cache
        auto range = m_HlslShaders.equal_range(shaderId);
        m_HlslShaders.erase(range.first, range.second);
    };

    void CDevice::OnAsmShaderDestructor(AsmShaderImpl* shader) {
        hta::CStr fName = shader->mFileName;
        UnifyFileName(&fName);

        auto range = m_AsmShaders.equal_range(fName);
        m_AsmShaders.erase(range.first, range.second);
    };

    void CDevice::rstShadersPrepareFor() {
        for (auto& pair : m_effects) {
            pair.second->OnDeviceReset();
        }
    };

    void CDevice::rstShadersRestoreAfter() {
        for (auto& pair : m_effects) {
            pair.second->OnDeviceRestore();
        }
    };

    void CDevice::ReleaseAsmShaders() {
        for (auto& pair : m_AsmShaders) {
            AsmShaderImpl* shader = pair.second;
            int refCount          = shader->GetRefCount();

            LOG_WARNING("Asm shader '%s' is not released (refs = %d)", shader->mFileName.m_charPtr, refCount);
        }
    };

    void CDevice::ReleaseHlslShaders() {
        for (auto& pair : m_HlslShaders) {
            HlslShaderImpl* shader = pair.second;
            int refCount           = shader->GetRefCount();

            LOG_WARNING("Hlsl shader '%s' is not released (refs = %d)", shader->mFileName.m_charPtr, refCount);
        }
    };

    void CDevice::ReleaseEffects() {
        for (auto& pair : m_effects) {
            EffectImpl* effect = pair.second;
            int refCount       = effect->GetRefCount();

            LOG_WARNING("fx shader '%s' is not released (refs = %d)", effect->mFileName.m_charPtr, refCount);
        }
    };

    void CDevice::createD3DXMacros() {
        m_d3dxMacros.resize(m_shadersMacros.size());

        for (size_t i = 0; i < m_shadersMacros.size(); ++i) {
            m_d3dxMacros[i].Name       = m_shadersMacros[i].macro.name.m_charPtr;
            m_d3dxMacros[i].Definition = m_shadersMacros[i].macro.definition.m_charPtr;
        }

        D3DXMACRO finishTag = {nullptr, nullptr};
        m_d3dxMacros.push_back(finishTag);
    };

    void CDevice::addMacro(const hta::CStr& name, const hta::CStr& definition, bool userDefined) {
        MacroData md;
        md.macro.name       = name;
        md.macro.definition = definition;
        md.userDefined      = userDefined;

        m_shadersMacros.push_back(md);
    };

    void CDevice::loadShadersMacros() {
        // Clear existing macros
        m_shadersMacros.clear();

        // Load user-defined macros from config (8 slots: 0x160 / 44 = 8)
        const hta::m3d::EngineConfig& config = hta::m3d::Kernel::Instance()->GetEngineCfg();
        for (int i = 0; i < 8; ++i) {
            const hta::CStr& macro = config.m_r_shadersMacros[i].m_s;

            if (macro.m_charPtr && strlen(macro.m_charPtr) > 0) {
                std::vector<hta::CStr> macroTokens;
                Tokenize(macro, macroTokens, "(), ;\t");

                if (macroTokens.size() == 2) {
                    addMacro(macroTokens[0], macroTokens[1], true);
                }
            }
        }

        if (IsNV3x()) {
            hta::CStr name("NV3x");
            hta::CStr value("true");
            addMacro(name, value, false);
        }

        {
            hta::CStr name("SUPPORT_DEPTH_TEXTURES");
            hta::CStr value("true");
            addMacro(name, value, false);
        }

        {
            hta::CStr name("MAX_ANISOTROPY");
            hta::CStr value(GetMaxAnisotropy());
            addMacro(name, value, false);
        }

        {
            hta::CStr name("MAX_INSTANCES");
            int maxInstances = (GetMaxVertexShaderConst() - 20) / 9;
            hta::CStr value(maxInstances);
            addMacro(name, value, false);
        }

        createD3DXMacros();
    };

    void CDevice::saveShadersMacros() {
        unsigned int configIdx = 0;

        // Save user-defined macros to config
        for (const auto& macroData : m_shadersMacros) {
            if (!macroData.userDefined)
                continue;

            const char* name = macroData.macro.name.m_charPtr;
            if (!name || strlen(name) == 0)
                continue;

            if (configIdx >= 8) // SHADERS_MACROS_NUM = 8 (0x160 / 44)
            {
                LOG_ERROR("i < SHADERS_MACROS_NUM");
                break;
            }

            // Build macro string: "NAME DEFINITION"
            hta::CStr macroStr = hta::CStr::format_("%s %s", macroData.macro.name.c_str(), macroData.macro.definition.c_str());

            // Get config variable for this slot
            hta::m3d::EngineConfig& config = hta::m3d::Kernel::Instance()->GetEngineCfg();
            hta::m3d::CVar& cvar           = config.m_r_shadersMacros[configIdx];

            // Only update if not read-only
            if ((cvar.m_flags & 2) == 0) {
                cvar.Set(macroStr.m_charPtr, false);
            }

            configIdx++;
        }

        // Clear remaining config slots
        for (unsigned int i = configIdx; i < 8; ++i) {
            hta::m3d::EngineConfig& config = hta::m3d::Kernel::Instance()->GetEngineCfg();
            hta::m3d::CVar& cvar           = config.m_r_shadersMacros[i];

            if ((cvar.m_flags & 2) == 0) {
                cvar.Set("", false);
            }
        }
    };

    void CDevice::ReleaseQueries() {
        for (auto* query : m_queries) {
            int refCount = query->GetRefCount();
            int type     = query->GetType();

            LOG_WARNING("Query with type %i is not released (refs = %d)", type, refCount);
        }
    };

    void CDevice::OnQueryDestructor(IQuery* query) {
        m_queries.remove(query);
    };

    void CDevice::rstQueryPrepareFor() {
        for (auto* query : m_queries) {
            Query* queryImpl = static_cast<Query*>(query);
            if (queryImpl->m_query) {
                queryImpl->m_query->Release();
            }
        }
    };

    void CDevice::rstQueryRestoreAfter() {
        for (auto* query : m_queries) {
            if (query->IsValid()) {
                Query* queryImpl   = static_cast<Query*>(query);
                queryImpl->m_state = IQuery::QUERY_SIGNALED; // Supported state

                m_pd3dDevice->CreateQuery(static_cast<D3DQUERYTYPE>(queryImpl->m_type), &queryImpl->m_query);
            }
        }
    };

    void CDevice::CreateFsRt() {};

    void CDevice::ReleaseFsRt() {};

    void CDevice::StartRenderingToFsRt() {};

    void CDevice::FinishRenderingToFsRt() {};

    void CDevice::DrawFsRt() {};

    int CDevice::SetupDXCursor(const TexHandle& texId, int xHotSpot, int yHotSpot, int frame) {
        // Check if cursor is already set up with these parameters
        if (m_currentDXCursorInfo.m_texId.m_handle == texId.m_handle && m_currentDXCursorInfo.m_xHotSpot == xHotSpot &&
            m_currentDXCursorInfo.m_yHotSpot == yHotSpot && m_currentDXCursorInfo.m_frame == frame) {
            return 1;
        }

        // Parameters changed, force update
        return SetupDXCursorForce(texId, xHotSpot, yHotSpot, frame);
    };

    void CDevice::MoveDXCursor(int x, int y) {
        if (!m_pd3dDevice)
            return;

        POINT coord = {x, y};

        // Convert to screen coordinates if in windowed mode
        const hta::m3d::CVar& fullScreen = hta::m3d::Kernel::Instance()->GetEngineCfg().m_r_fullScreen;
        bool isFullScreen                = (fullScreen.m_type == hta::m3d::CVar::CVAR_BOOL) ? fullScreen.m_b : (fullScreen.m_i > 0);

        if (!isFullScreen) {
            ClientToScreen(m_d3dpp.hDeviceWindow, &coord);
        }

        m_pd3dDevice->SetCursorPosition(coord.x, coord.y, D3DCURSOR_IMMEDIATE_UPDATE);
    };

    void CDevice::ShowDXCursor(bool needShow) {
        if (m_pd3dDevice) {
            UpdateDXCursorFrame();
            m_pd3dDevice->ShowCursor(needShow);
        }
    };

    int CDevice::UpdateDXCursorFrame() {
        if (!m_pd3dDevice)
            return 0;

        if (!IsTexValid(m_currentDXCursorInfo.m_texId))
            return 0;

        int currentFrame = GetTextureCurrentFrame(m_currentDXCursorInfo.m_texId, -1.0f);

        if (currentFrame == -1)
            return 0;

        // If frame hasn't changed, nothing to do
        if (currentFrame == m_currentDXCursorInfo.m_frame)
            return 1;

        // Frame changed, update cursor
        return SetupDXCursor(
            m_currentDXCursorInfo.m_texId, m_currentDXCursorInfo.m_xHotSpot, m_currentDXCursorInfo.m_yHotSpot, currentFrame
        );
    };

    bool CDevice::IsHardWareCursorAvailableForTexture(const TexHandle& texId) const {
        int w = 0;
        int h = 0;
        GetDims(texId, w, h);

        // Hardware cursor requires specific capabilities and 32x32 size
        bool hasHardwareCursor = (m_d3dCaps.CursorCaps & D3DCURSORCAPS_COLOR) != 0;
        bool isCorrectSize     = (w == 32 && h == 32);

        if (!hasHardwareCursor || !isCorrectSize) {
            // In windowed mode, we can use software cursor fallback
            const hta::m3d::CVar& fullScreen = hta::m3d::Kernel::Instance()->GetEngineCfg().m_r_fullScreen;
            bool isFullScreen                = (fullScreen.m_type == hta::m3d::CVar::CVAR_BOOL) ? fullScreen.m_b : (fullScreen.m_i > 0);

            if (isFullScreen)
                return FALSE;
        }

        return TRUE;
    };

    bool CDevice::IsNV3x() const {
        return false;
    };

    void CDevice::SingleLayerStencilStart() {
        int32_t& stencilLevel = m_stencilLevel[m_activeStencilTarget];

        // Clear stencil buffer if starting fresh (high bit set indicates uninitialized)
        if ((stencilLevel & 0x80000000) != 0) {
            ClearViewport(M3DCLEAR_S, 0xFFFFFFFF);
        }

        // Increment stencil level
        ++stencilLevel;

        // Configure stencil operations
        SetStencilPass(OP_REPLACE, false);
        SetStencilZFail(OP_KEEP, false);
        SetStencilFail(OP_KEEP, false);
        SetStencilFunc(M3DCMP_GREATER, false);
        SetStencilRef(stencilLevel, false);
        SetStencilMask(0xFFFFFFFF, false);
        SetStencilWriteMask(0xFFFFFFFF, false);

        // Enable stencil testing
        if (m_curRenderState[52] != 1) {
            ++m_stats.swRenderStates;
            m_curRenderState[52] = 1;
            m_pd3dDevice->SetRenderState(D3DRS_STENCILENABLE, TRUE);
        }
    };

    void CDevice::SingleLayerStencilContinue() {
        int32_t& stencilLevel = m_stencilLevel[m_activeStencilTarget];

        // Ensure we're in an active stencil layer
        if ((int)stencilLevel <= 0) {
            LOG_ERROR("level > 0");
            return;
        }

        // Configure stencil operations (same as Start, but without incrementing level)
        SetStencilPass(OP_REPLACE, false);
        SetStencilZFail(OP_KEEP, false);
        SetStencilFail(OP_KEEP, false);
        SetStencilFunc(M3DCMP_GREATER, false);
        SetStencilRef(stencilLevel, false);
        SetStencilMask(0xFFFFFFFF, false);
        SetStencilWriteMask(0xFFFFFFFF, false);

        // Enable stencil testing
        if (m_curRenderState[52] != 1) {
            ++m_stats.swRenderStates;
            m_curRenderState[52] = 1;
            m_pd3dDevice->SetRenderState(D3DRS_STENCILENABLE, TRUE);
        }
    };

    void CDevice::SingleLayerStencilFinish() {
        // Disable stencil testing
        if (m_curRenderState[52] != 0) {
            ++m_stats.swRenderStates;
            m_curRenderState[52] = 0;
            m_pd3dDevice->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        }
    };
    int CDevice::SetupDXCursorForce(const TexHandle& texId, int xHotSpot, int yHotSpot, int frame) {
        if (!m_pd3dDevice)
            return 0;

        if (!IsTexValid(texId))
            return 0;

        Sampler* texture = this->mActiveTextures.GetItem(texId.m_handle);

        if (!texture->m_maps[0]->Is2D())
            return 0;

        // Clamp frame to valid range
        int validFrame = frame;
        if (frame < 0 || frame >= (int)texture->m_maps.size()) {
            validFrame = 0;
        }

        // Get surface from texture
        IDirect3DSurface9* surface = nullptr;
        if (FAILED(texture->m_maps[validFrame]->mHandle2D->GetSurfaceLevel(0, &surface)))
            return 0;

        // Set cursor properties
        if (FAILED(m_pd3dDevice->SetCursorProperties(xHotSpot, yHotSpot, surface))) {
            if (surface)
                surface->Release();
            return 0;
        }

        if (surface)
            surface->Release();

        // Update texture reference if changed
        if (m_currentDXCursorInfo.m_texId.m_handle != texId.m_handle) {
            ReleaseTexture(m_currentDXCursorInfo.m_texId);
            ReferenceTexture(texId);
        }

        // Update cursor info
        m_currentDXCursorInfo.m_texId.m_handle = texId.m_handle;
        m_currentDXCursorInfo.m_xHotSpot       = xHotSpot;
        m_currentDXCursorInfo.m_yHotSpot       = yHotSpot;
        m_currentDXCursorInfo.m_frame          = validFrame;

        return 1;
    };

    int32_t CDevice::GetTextureCurrentFrame(const TexHandle& texId, double tsc) {
        if (texId.m_handle >= 0) {
            return GetTextureCurrentFrame(this->mActiveTextures.GetItem(texId.m_handle), tsc);
        } else {
            return -1;
        }
    };

    int32_t CDevice::GetTextureCurrentFrame(Sampler* tex, double tsc) {
        float normalizedTime = tsc;

        // Auto-calculate time for animated textures
        if (tsc < 0.0f && tex->m_fps > 0) {
            int currentTime = hta::m3d::Kernel::Instance()->GetTimer().m_frameStartTimeUnscaled;
            int numFrames   = tex->m_maps.size();

            if (tex->m_looped) {
                // Looping animation - use modulo to wrap around
                int cycleDuration = (1000 * numFrames) / tex->m_fps;
                int timeInCycle   = currentTime % cycleDuration;
                normalizedTime    = (float)timeInCycle / (float)cycleDuration;
            } else {
                // One-shot animation - clamp to duration
                if (tex->m_timeStamp == 0.0) {
                    tex->m_timeStamp = (double)currentTime;
                }

                float duration = (float)numFrames / (float)tex->m_fps;
                float elapsed  = (float)(currentTime - (int)tex->m_timeStamp) / 1000.0f;

                if (elapsed > duration)
                    elapsed = duration;

                normalizedTime = elapsed / duration;
            }
        }

        // Return first frame if time not specified
        if (normalizedTime == -1.0f)
            return 0;

        // Convert normalized time [0,1] to frame index
        int numFrames = tex->m_maps.size();
        int frame     = (int)((float)numFrames * normalizedTime);

        // Clamp to valid range
        if (frame < 0)
            frame = 0;
        if (frame > numFrames - 1)
            frame = numFrames - 1;

        return frame;
    };

    int32_t CDevice::SetupAllFeaturesSupport(const hta::CStr& DevCompatibleFileName) {
        // Get device capabilities
        _SetLastResult(m_pd3dDevice->GetDeviceCaps(&m_d3dCaps));

        // Check stencil buffer support
        IDirect3DSurface9* depthStencil = nullptr;
        _SetLastResult(m_pd3dDevice->GetDepthStencilSurface(&depthStencil));

        D3DSURFACE_DESC surfaceDesc;
        depthStencil->GetDesc(&surfaceDesc);
        depthStencil->Release();

        bool hasStencil =
            (surfaceDesc.Format == D3DFMT_D15S1 || surfaceDesc.Format == D3DFMT_D24S8 || surfaceDesc.Format == D3DFMT_D24X4S4);

        // Require SM 3.0 minimum
        WORD vsVersion = LOWORD(m_d3dCaps.VertexShaderVersion);
        WORD psVersion = LOWORD(m_d3dCaps.PixelShaderVersion);

        if (vsVersion < 0x0300 || psVersion < 0x0300) {
            LOG_ERROR("This graphics card does not support Shader Model 3.0 (minimum requirement)");
            return 0; // Unsupported
        }

        // Set shader model features (always true since we require SM 3.0+)
        m_featureSupported[FEATURE_VS_1_1] = true;
        m_featureSupported[FEATURE_VS_2_0] = true;
        m_featureSupported[FEATURE_VS_3_0] = true;
        m_featureSupported[FEATURE_PS_1_1] = true;
        m_featureSupported[FEATURE_PS_1_4] = true;
        m_featureSupported[FEATURE_PS_2_0] = true;
        m_featureSupported[FEATURE_PS_3_0] = true;

        // Set other features
        m_featureSupported[FEATURE_STENCIL]              = hasStencil;
        m_featureSupported[FEATURE_2SIDED_STENCIL]       = hasStencil && (m_d3dCaps.StencilCaps & D3DSTENCILCAPS_TWOSIDED);
        m_featureSupported[FEATURE_NON_POW2_CONDITIONAL] = (m_d3dCaps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL) != 0;
        m_featureSupported[0x0019]                       = defineInstancingSupport();
        m_featureSupported[0x001a]                       = (m_texFormatDepth != D3DFMT_UNKNOWN);

        // Detect query support
        // detectQuerySupport();

        return 1;
    };

    void CDevice::rstStencilLevels() {
        m_stencilLevel[0] = -1;
        m_stencilLevel[1] = -1;
    };

    void CDevice::ResetTextureStates() {
        struct SamplerStateReset {
            int index;
            D3DSAMPLERSTATETYPE state;
            DWORD defaultValue;
        };

        static const SamplerStateReset statesToReset[] = {
            {4, D3DSAMP_BORDERCOLOR, 0},
            {8, D3DSAMP_MIPMAPLODBIAS, 0},
            {9, D3DSAMP_MAXMIPLEVEL, 0},
            {10, D3DSAMP_MAXANISOTROPY, 1},
            {11, D3DSAMP_SRGBTEXTURE, 0},
            {12, D3DSAMP_ELEMENTINDEX, 0},
            {13, D3DSAMP_DMAPOFFSET, 256}
        };

        for (int stage = 0; stage < 8; ++stage) {
            for (const auto& reset : statesToReset) {
                if (m_curTexSamplerStates[stage][reset.index] != reset.defaultValue) {
                    ++m_stats.swTextureSamplerStates;
                    m_curTexSamplerStates[stage][reset.index] = reset.defaultValue;
                    m_pd3dDevice->SetSamplerState(stage, reset.state, reset.defaultValue);
                }
            }
        }
    };

    void CDevice::SetRenderThreadId(unsigned int renderThreadId) {
        m_renderThreadId = renderThreadId;
    };

    void CDevice::EnableThreadSafeQuard(bool enable) {
        m_bThreadSafeGuardEnabled = enable;
    };

    CDevice* CDevice::Instance(void) {
        return G_DEVICE;
    };
};