#define LOGGER "KINEMATICFRICTION"

#include <math.h>

#include "ode/ode.hpp"
#include "hta/m3d/Object.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/ai/Wheel.hpp"
#include "hta/ai/CServer.hpp"
#include "hta/ai/DynamicScene.hpp"
#include "hta/m3d/WheelTraceMgr.hpp"
#include "hta/m3d/RoadNode.hpp"
#include "hta/m3d/CWorld.hpp"
#include "hta/m3d/Landscape.hpp"
#include "hta/m3d/GeomObjectRoad.hpp"

#include "ext/logger.hpp"
#include "routines.hpp"

#include "fix/kineticfriction.hpp"

namespace kraken::fix::kineticfriction {
    // TireParams/calculateKappa/mu_from_kappa now live in fix/kineticfriction.hpp, shared with
    // fix::joltshadow's SetTireMaxImpulseCallback (docs §23.8).

    const auto CollideWheelDefault = (int32_t (__fastcall*)(hta::ai::Wheel*, hta::m3d::Object*, dContact*, uint32_t*, bool))(0x00891430);

    static TireParams DUMMY_TIRES;

    inline hta::CVector _CalculateTracePosition(dContact* contact) {
        hta::CVector pos;
        pos  = hta::CVector(contact->geom.normal);
        pos *= 0.01;
        pos += hta::CVector(contact->geom.pos);
        return pos;
    };

    int __fastcall CollideWheelSurface(hta::ai::Wheel* wheel, int32_t soil_id, float friction, dContact* contact, uint32_t* count, bool reverse) {
        hta::m3d::WheelTraceMgr& trace_manager = hta::ai::CServer::Instance()->GetWorld()->GetWheelTracesMgr();

        if (!count || *count == 0 || !contact) {
            if (trace_manager.IsSkiddingStarted(wheel)) {
                trace_manager.EndSkidding(wheel, true);
            }
            return 1;
        }

        hta::CVector    wheel_angular   = hta::CVector(dBodyGetAngularVel(wheel->m_body->_id));
        hta::Quaternion wheel_rotation  = wheel->GetRotation();
        hta::Quaternion invert_rotation = ~wheel_rotation;
        float      wheel_radius    = wheel->GetRadius();

        hta::ai::Vehicle*                  vehicle         = wheel->GetVehicle();
        const hta::ai::WheelPrototypeInfo* info            = wheel->GetPrototypeInfo();
        hta::CVector                       local_angular   = invert_rotation * wheel_angular;
        hta::CVector                       world_linear    = wheel->GetLinearVelocity();
        float                              omega_parallel  = local_angular.x;
        float                              V_parallel      = world_linear.Length();
        float                              kappa           = calculateKappa(wheel_radius, omega_parallel, V_parallel, 0.05f);
        float                              mu_longitudinal = mu_from_kappa(kappa, DUMMY_TIRES);
        float                              mu_lateral      = mu_longitudinal * DUMMY_TIRES.lateral_factor;

        mu_longitudinal *= info->m_mU * friction;
        mu_lateral      *= info->m_mU * friction;

        bool in_skid = false;
        if (vehicle && vehicle->m_onOilMode) {
            mu_longitudinal *= DUMMY_TIRES.oil_factor;
            mu_lateral      *= DUMMY_TIRES.oil_factor;
            in_skid          = true;
        }
        else {
            in_skid = abs(kappa) > 0.75f;
            // TODO: Dust effect
            // TODO: Smoke effect
        };

        if (in_skid) {
            hta::CVector pos = _CalculateTracePosition(contact);

            if (trace_manager.IsSkiddingStarted(wheel)) {
                trace_manager.AddTrace(pos, wheel_rotation, wheel->GetWidth(), wheel, soil_id, false);
            } else {
                trace_manager.StartSkidding(wheel, soil_id);
                trace_manager.AddTrace(pos, wheel_rotation, wheel->GetWidth(), wheel, soil_id, true);
            }
        } else if (trace_manager.IsSkiddingStarted(wheel)) {
            trace_manager.EndSkidding(wheel, true);
        }

        for (size_t idx = 0; idx < *count; idx++) {
            contact[idx].surface.mu  = mu_longitudinal;
            contact[idx].surface.mu2 = mu_lateral;
        };

        return 1;
    };

    int __fastcall CollideWheelAndAsphalt(hta::ai::Wheel* wheel, hta::m3d::Object* object, dContact* contact, uint32_t* count, bool reverse) {
        CollideWheelDefault(wheel, object, contact, count, reverse);
        hta::m3d::GeomObjectRoad* road = reinterpret_cast<hta::m3d::GeomObjectRoad*>(object);
        hta::m3d::RoadNode* surface = reinterpret_cast<hta::m3d::RoadNode*>(road->m_roadNode);
        return CollideWheelSurface(wheel, surface->GetSoilType(), 1.0f, contact, count, reverse);
    };

    int __fastcall CollideWheelAndLandscape(hta::ai::Wheel* wheel, hta::m3d::Object* ground, dContact* contact, uint32_t* count, bool reverse) {
        CollideWheelDefault(wheel, ground, contact, count, reverse);
 
        hta::m3d::Landscape& terrain = hta::ai::CServer::Instance()->GetWorld()->GetLandscape();
        float tile_size = (float) hta::ai::CServer::Instance()->GetLevelSize() / (float)terrain.GetTileSize();
        
        int32_t x = (int32_t)(contact->geom.pos[0] / tile_size + 0.5);
        int32_t z = (int32_t)(contact->geom.pos[2] / tile_size + 0.5);

        const hta::ai::DynamicScene::SoilProps& props = hta::ai::DynamicScene::Instance()->GetSoilProps(x, z);

        return CollideWheelSurface(wheel, props.m_idx, props.m_friction, contact, count, reverse);
    };

    void Apply() {
        routines::Redirect(0x0729, (void*)0x00891680, &CollideWheelAndAsphalt);
        routines::Redirect(0x0971, (void*)0x00891DB0, &CollideWheelAndLandscape);
    };
};