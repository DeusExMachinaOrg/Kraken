#pragma once

#include "hta/CStr.h"
#include "hta/NFloat4.hpp"
#include "hta/CVector.h"
#include "hta/CMatrix.hpp"
#include "hta/m3d/rend/Common.hpp"
#include "hta/m3d/rend/TexHandle.hpp"
#include "hta/m3d/rend/IRenderResource.hpp"

#include <stdint.h>

namespace m3d::rend {


    struct IEffect : public IRenderResource {
        struct TechniqueDesc {
            /* Size=0x30 */
            /* 0x0000 */ CStr name;
            /* 0x000c */ uint32_t numPasses;
            /* 0x0010 */ CStr briefDesc;
            /* 0x001c */ CStr vertexFormatStr;
            /* 0x0028 */ VertexType vertexFormat;
            /* 0x002c */ bool tangentSpaceUsed;
            /* 0x002d */ bool isDefault;
            /* 0x002e */ bool isPS20;
            /* 0x002f */ bool useAlpha;
            
            TechniqueDesc(const TechniqueDesc&);
            TechniqueDesc();
            ~TechniqueDesc();
        };

        enum Parameter : int32_t {
            World = 0x0000,
            View = 0x0001,
            Projection = 0x0002,
            ModelView = 0x0003,
            InvWorld = 0x0004,
            ModelViewProjection = 0x0005,
            ViewPos = 0x0006,
            DiffMap0 = 0x0007,
            CubeMap0 = 0x0008,
            BumpMap0 = 0x0009,
            DetailMap0 = 0x000a,
            LightMap0 = 0x000b,
            NormalizationCubemap = 0x000c,
            Time_Linear = 0x000d,
            Tree_Bend_Term = 0x000e,
            LightAmbient = 0x000f,
            LightDiffuse = 0x0010,
            LightPlant = 0x0011,
            LightSpecular = 0x0012,
            FogTerm = 0x0013,
            Transparency = 0x0014,
            TransStartDist = 0x0015,
            TransObjectWidth = 0x0016,
            TmpLight0Dir = 0x0017,
            User_float_param = 0x0018,
            User_float_param2 = 0x0019,
            User_float_param3 = 0x001a,
            User_float3_param = 0x001b,
            User_float3_param2 = 0x001c,
            User_float4_param = 0x001d,
            User_float4x4_param = 0x001e,
            NumParameters = 0x001f,
            InvalidParameter = 0x0020,
        };

        /* Size=0x8 */
        /* 0x0000: fields for IRenderResource */
    
        virtual uint32_t GetNumTechniques() const;
        virtual const TechniqueDesc& GetTechniqueDesc(uint32_t) const;
        virtual void SetCurTechnique(uint32_t);
        virtual uint32_t GetCurTechnique() const;
        virtual void SetDefaultTechnique(bool);
        virtual bool IsParameterUsed(Parameter);
        virtual void SetInt(Parameter, int32_t);
        virtual void SetFloat(Parameter, float);
        virtual void SetVector4(Parameter, const CVector4&);
        virtual void SetVector3(Parameter, const CVector&);
        virtual void SetFloat4(Parameter, const nFloat4&);
        virtual void SetMatrix(Parameter, const CMatrix&);
        virtual void SetTexture(Parameter, TexHandle*);
        virtual void SetIntArray(Parameter, const int32_t*, int32_t);
        virtual void SetFloatArray(Parameter, const float*, int32_t);
        virtual void SetFloat4Array(Parameter, const nFloat4*, int32_t);
        virtual void SetVector4Array(Parameter, const CVector4*, int32_t);
        virtual void SetMatrixArray(Parameter, const CMatrix*, int32_t);
        virtual void SetMatrixPointerArray(Parameter, const CMatrix**, int32_t);
        virtual ~IEffect();
        IEffect(const IEffect&);
        IEffect();
    };
};