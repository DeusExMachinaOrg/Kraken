#pragma once

#include "hta/m3d/rend/Common.hpp"
#include "hta/m3d/rend/IRenderResource.hpp"

#include <stdint.h>

namespace m3d::rend {
    struct QueryReturnValue {
        enum Type : int32_t {
            NotValid = 0x0000,
            Bool = 0x0001,
            Dword = 0x0002,
            Uint64 = 0x0003,
            BandWidthTimings = 0x0004,
            CacheUtilization = 0x0005,
            InterfaceTimings = 0x0006,
            PipelineTimings = 0x0007,
            StageTimings = 0x0008,
            VCache = 0x0009,
        };

        /* Size=0x20 */
        /* 0x0000 */ Type m_type;
        union {
            /* 0x0008 */ bool b;
            /* 0x0008 */ uint32_t d;
            /* 0x0008 */ uint64_t i64;
            /* 0x0008 */ BANDWIDTHTIMINGS bw;
            /* 0x0008 */ CACHEUTILIZATION cu;
            /* 0x0008 */ INTERFACETIMINGS it;
            /* 0x0008 */ PIPELINETIMINGS pt;
            /* 0x0008 */ STAGETIMINGS st;
            /* 0x0008 */ VCACHE vc;
        };
    
        QueryReturnValue();
        ~QueryReturnValue();
        bool operator==(const QueryReturnValue&) const;
        void SetType(Type);
        Type GetType() const;
        bool IsValid() const;
        bool GetBool() const;
        uint32_t GetDword() const;
        uint64_t GetUInt64() const;
        const BANDWIDTHTIMINGS& GetBandWidthTimings() const;
        const CACHEUTILIZATION& GetCacheUtilization() const;
        const INTERFACETIMINGS& GetInterfaceTimings() const;
        const PIPELINETIMINGS& GetPipelineTimings() const;
        const STAGETIMINGS& GetStageTimings() const;
        const VCACHE& GetVCash() const;
    };

    struct IQuery : public IRenderResource {
        enum State : int32_t {
            QUERY_NOT_SUPPORT = 0x0000,
            QUERY_SIGNALED = 0x0001,
            QUERY_ISSUED = 0x0002,
            QUERY_ERROR = 0x0003,
        };

        enum Type : int32_t {
            QUERY_VCACHE = 0x0004,
            QUERY_EVENT = 0x0008,
            QUERY_OCCLUSION = 0x0009,
            QUERY_TIMESTAMP = 0x000a,
            QUERY_TIMESTAMPDISJOINT = 0x000b,
            QUERY_TIMESTAMPFREQ = 0x000c,
            QUERY_PIPELINETIMINGS = 0x000d,
            QUERY_INTERFACETIMINGS = 0x000e,
            QUERY_VERTEXTIMINGS = 0x000f,
            QUERY_BANDWIDTHTIMINGS = 0x0011,
            QUERY_CACHEUTILIZATION = 0x0012,
        };
        /* Size=0x8 */
        /* 0x0000: fields for IRenderResource */

        virtual Type GetType() const;
        virtual void Begin();
        virtual void End();
        virtual State GetState();
        virtual const QueryReturnValue &GetData();
        virtual ~IQuery();
        IQuery(const IQuery &);
        IQuery();
    };

};