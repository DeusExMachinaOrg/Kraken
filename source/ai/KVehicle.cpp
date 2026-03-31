#include "ai/KVehicle.hpp"
#include <new>

namespace hta {
    m3d::Object* ai::KVehicle::CreateObject() {
        m3d::Kernel::Instance()->SysError("!\"Object cannot be created directly\"", ".\\ai\\KVehicle.cpp");
        return nullptr;
    }

    m3d::Class* ai::KVehicle::GetBaseClass() {
        return ai::Vehicle::GetClassObject();
    }

    ai::Obj* ai::KVehiclePrototypeInfo::CreateTargetObject() const {
        ai::KVehicle* result = (ai::KVehicle*)hta::m3d::Kernel::Instance()->g_mar.AllocMem(sizeof(ai::KVehicle), nullptr, 0);

        using VehicleCtor = void(__thiscall*)(ai::Vehicle*, const ai::VehiclePrototypeInfo&);
        static auto ctor = reinterpret_cast<VehicleCtor>(0x005ED590);
        ctor(reinterpret_cast<ai::Vehicle*>(result), *this);

        ::new (&result->m_sumSurfaceVelocities) CVector();

        return result;
    }

    m3d::Class* ai::KVehicle::GetClass() const {
        return GetClassObject();
    }
}