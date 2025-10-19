#pragma once

#include "hta/CVector.h"
#include "hta/CVector4.hpp"
#include "hta/CMatrix.hpp"
#include "hta/NFloat4.hpp"
#include "hta/m3d/rend/Common.hpp"
#include "hta/m3d/rend/IRenderResource.hpp"

namespace m3d::rend {
    struct IHlslShader : public IRenderResource {
        enum Profile : int32_t {
            VS_1_1 = 0x0000,
            VS_2_0 = 0x0001,
            VS_3_0 = 0x0002,
            PS_1_1 = 0x0003,
            PS_1_3 = 0x0004,
            PS_1_4 = 0x0005,
            PS_2_0 = 0x0006,
            PS_2_a = 0x0007,
            PS_3_0 = 0x0008,
        };
        /* Size=0x8 */
        /* 0x0000: fields for IRenderResource */

        static const uint32_t INVALID_PARAM;

        virtual uint32_t GetNumberOfParams() const;
        virtual uint32_t GetParamHandleByName(const char*);
        virtual void SetInt(uint32_t, int32_t);
        virtual void SetFloat(uint32_t, float);
        virtual void SetVector4(uint32_t, const CVector4&);
        virtual void SetVector3(uint32_t, const CVector&);
        virtual void SetFloat4(uint32_t, const nFloat4&);
        virtual void SetMatrix(uint32_t, const CMatrix&);
        virtual void SetIntArray(uint32_t, const int32_t*, int32_t);
        virtual void SetFloatArray(uint32_t, const float*, int32_t);
        virtual void SetFloat4Array(uint32_t, const nFloat4*, int32_t);
        virtual void SetVector4Array(uint32_t, const CVector4*, int32_t);
        virtual void SetMatrixArray(uint32_t, const CMatrix*, int32_t);
        virtual void SetMatrixPointerArray(uint32_t, const CMatrix**, int32_t);
        virtual void Apply();
        virtual ~IHlslShader();
        IHlslShader(const IHlslShader&);
        IHlslShader();
    };
};