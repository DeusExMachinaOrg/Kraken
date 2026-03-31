#define LOGGER "METAINFORMATION"

#include "ext/meta.hpp"

#include "ext/logger.hpp"
#include "routines.hpp"

#include "hta/CStr.hpp"
#include "hta/ai/Obj.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/ai/CServer.hpp"

namespace kraken::meta {
    hta::ai::PrototypeInfo* __fastcall CreatePrototypeInfoByClassName(hta::ai::CServer& self, void*, const hta::CStr& className) {
        hta::ai::PrototypeInfo* result = nullptr;
        if (className == "KVehicle")
        {
            result = (hta::ai::PrototypeInfo*)hta::m3d::Kernel::Instance()->g_mar.AllocMem(300, 0, 0);
            if ( result ) {
                using VehiclePrototypeInfoCtor = void(__thiscall*)(hta::ai::VehiclePrototypeInfo*);
                static auto ctor = reinterpret_cast<VehiclePrototypeInfoCtor>(0x005E5040);
                ctor(reinterpret_cast<hta::ai::VehiclePrototypeInfo*>(result));
            }
        }
        else {
            result = self.CreatePrototypeInfoByClassName(className);
        }
        return result;
    }

    void Init() {
        routines::ChangeCall((void*) 0x006E84AD, CreatePrototypeInfoByClassName);
    }
}