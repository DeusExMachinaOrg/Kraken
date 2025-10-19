#pragma once

#include <stdint.h>

namespace m3d::rend {
    enum VertexType : int32_t {
        VERTEX_XYZ = 0x0000,
        VERTEX_XYZT1 = 0x0001,
        VERTEX_XYZC = 0x0002,
        VERTEX_XYZWC = 0x0003,
        VERTEX_XYZWCT1 = 0x0004,
        VERTEX_XYZNC = 0x0005,
        VERTEX_XYZCT1 = 0x0006,
        VERTEX_XYZNT1 = 0x0007,
        VERTEX_XYZNCT1 = 0x0008,
        VERTEX_XYZNCT2 = 0x0009,
        VERTEX_XYZNT2 = 0x000a,
        VERTEX_XYZNT3 = 0x000b,
        VERTEX_XYZCT1_UVW = 0x000c,
        VERTEX_XYZCT2_UVW = 0x000d,
        VERTEX_XYZCT2 = 0x000e,
        VERTEX_XYZNT1T = 0x000f,
        VERTEX_XYZNCT1T = 0x0010,
        VERTEX_XYZNCT1_UV2_S1 = 0x0011,
        VERTEX_STREAM_UV_S1 = 0x0012,
        VERTEX_WATERTEST = 0x0013,
        VERTEX_GRASSTEST = 0x0014,
        VERTEX_IMPOSTORTEST = 0x0015,
        VERTEX_YNI = 0x0016,
        VERTEX_XYZT1I = 0x0017,
    };

    enum Cull : int32_t {
        M3DCULL_NONE = 0x0000,
        M3DCULL_CW = 0x0001,
        M3DCULL_CCW = 0x0002,
    };


    struct BANDWIDTHTIMINGS {
        /* Size=0x14 */
        /* 0x0000 */ float MaxBandwidthUtilized;
        /* 0x0004 */ float FrontEndUploadMemoryUtilizedPercent;
        /* 0x0008 */ float VertexRateUtilizedPercent;
        /* 0x000c */ float TriangleSetupRateUtilizedPercent;
        /* 0x0010 */ float FillRateUtilizedPercent;
    };

    struct CACHEUTILIZATION {
        /* Size=0x8 */
        /* 0x0000 */ float TextureCacheHitRate;
        /* 0x0004 */ float PostTransformVertexCacheHitRate;
    };

    struct INTERFACETIMINGS {
        /* Size=0x14 */
        /* 0x0000 */ float WaitingForGPUToUseApplicationResourceTimePercent;
        /* 0x0004 */ float WaitingForGPUToAcceptMoreCommandsTimePercent;
        /* 0x0008 */ float WaitingForGPUToStayWithinLatencyTimePercent;
        /* 0x000c */ float WaitingForGPUExclusiveResourceTimePercent;
        /* 0x0010 */ float WaitingForGPUOtherTimePercent;
    };

    struct PIPELINETIMINGS {
        /* Size=0x10 */
        /* 0x0000 */ float VertexProcessingTimePercent;
        /* 0x0004 */ float PixelProcessingTimePercent;
        /* 0x0008 */ float OtherGPUProcessingTimePercent;
        /* 0x000c */ float GPUIdleTimePercent;
    };

    struct STAGETIMINGS {
        /* Size=0x8 */
        /* 0x0000 */ float MemoryProcessingPercent;
        /* 0x0004 */ float ComputationProcessingPercent;
    };

    struct VCACHE {
        /* Size=0x10 */
        /* 0x0000 */ uint32_t Pattern;
        /* 0x0004 */ uint32_t OptMethod;
        /* 0x0008 */ uint32_t CacheSize;
        /* 0x000c */ uint32_t MagicNumber;
    };

};