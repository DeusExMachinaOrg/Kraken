#define LOGGER "KINEMATICFRICTION"

#include <math.h>

#include "ode/ode.hpp"
#include "hta/m3d/Object.h"
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
    struct TireParams {
        float mu_peak        = 1.50f; // Пик динамического трения
        float mu_min         = 0.50f; // Хвост при полном скольжении
        float xg             = 0.06f; // Параметр нарастания к пику
        float q              = 2.00f; // Экспонента роста
        float xd             = 0.12f; // Начало спада
        float p              = 3.00f; // Экспонента спада
        float mu_cap         = 4.50f; // Потолок (защита от артефактов)
        float x0             = 0.01f; // Зона статического трения
        float r0             = 2.00f; // Резкость перехода статика→динамика
        float mu_static      = 3.00f; // Статическое трение (покой/идеальное качение)
        float lateral_factor = 1.20f; // Множитель перпендикулярного скольжения
        float oil_factor     = 0.05f; // Фактор трения на масле
    };

    inline float calculateKappa(float wheelRadius,
                                float omega_parallel,
                                float V_parallel,
                                float eps = 0.05f) {
        const float Rw = wheelRadius * omega_parallel;
        const float V_abs = fabsf(V_parallel);
        const float Rw_abs = fabsf(Rw);

        const float denom = fmaxf(fmaxf(V_abs, Rw_abs), eps);
        const float kappa = (Rw - V_parallel) / denom;

        // Single-line branchless blend
        const float t = fmaxf(0.0f, fminf(1.0f, (V_abs - 0.1f) / 0.9f));
        return kappa * t * t * (3.0f - 2.0f * t);
    };

    inline float mu_from_kappa(float kappa, const TireParams& t) {
        const float x = fabsf(kappa);
        const float G = 1.0f - expf(-powf(fmaxf(x, 1e-6f) / t.xg, t.q));
        const float D = 1.0f / (1.0f + powf(x / t.xd, t.p));
        float mu = t.mu_min + (t.mu_peak - t.mu_min) * (G * D);
        const float w0 = 1.0f - expf(-powf(x / t.x0, t.r0));
        mu = w0 * mu + (1.0f - w0) * t.mu_static;
        return fminf(mu, t.mu_cap);
    };


    const auto CollideWheelDefault = (int32_t (__fastcall*)(ai::Wheel*, m3d::Object*, dContact*, uint32_t*, bool))(0x00891430);

    static TireParams DUMMY_TIRES;

    inline CVector _CalculateTracePosition(dContact* contact) {
        CVector pos;
        pos  = CVector(contact->geom.normal);
        pos *= 0.01;
        pos += CVector(contact->geom.pos);
        return pos;
    };

    int __fastcall CollideWheelSurface(ai::Wheel* wheel, int32_t soil_id, float friction, dContact* contact, uint32_t* count, bool reverse) {
        m3d::WheelTraceMgr& trace_manager = ai::CServer::Instance()->GetWorld()->GetWheelTracesMgr();

        if (!count || *count == 0 || !contact) {
            if (trace_manager.IsSkiddingStarted(wheel)) {
                trace_manager.EndSkidding(wheel, true);
            }
            return 1;
        }

        CVector    wheel_angular   = CVector(dBodyGetAngularVel(wheel->m_body->_id));
        Quaternion wheel_rotation  = wheel->GetRotation();
        Quaternion invert_rotation = ~wheel_rotation;
        float      wheel_radius    = wheel->GetRadius();

        ai::Vehicle*                  vehicle         = wheel->GetVehicle();
        const ai::WheelPrototypeInfo* info            = wheel->GetPrototypeInfo();
        CVector                       local_angular   = invert_rotation * wheel_angular;
        CVector                       world_linear    = wheel->GetLinearVelocity();
        float                         omega_parallel  = local_angular.x;
        float                         V_parallel      = world_linear.length();
        float                         kappa           = calculateKappa(wheel_radius, omega_parallel, V_parallel, 0.05f);
        float                         mu_longitudinal = mu_from_kappa(kappa, DUMMY_TIRES);
        float                         mu_lateral      = mu_longitudinal * DUMMY_TIRES.lateral_factor;

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
            CVector pos = _CalculateTracePosition(contact);

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

    int __fastcall CollideWheelAndAsphalt(ai::Wheel* wheel, m3d::Object* object, dContact* contact, uint32_t* count, bool reverse) {
        CollideWheelDefault(wheel, object, contact, count, reverse);
        m3d::GeomObjectRoad* road = reinterpret_cast<m3d::GeomObjectRoad*>(object);
        m3d::RoadNode* surface = reinterpret_cast<m3d::RoadNode*>(road->m_roadNode);
        return CollideWheelSurface(wheel, surface->GetSoilType(), 1.0f, contact, count, reverse);
    };

    int __fastcall CollideWheelAndLandscape(ai::Wheel* wheel, m3d::Object* ground, dContact* contact, uint32_t* count, bool reverse) {
        CollideWheelDefault(wheel, ground, contact, count, reverse);
 
        m3d::Landscape& terrain = ai::CServer::Instance()->GetWorld()->GetLandscape();
        float tile_size = (float) ai::CServer::Instance()->GetLevelSize() / (float)terrain.GetTileSize();
        
        int32_t x = (int32_t)(contact->geom.pos[0] / tile_size + 0.5);
        int32_t z = (int32_t)(contact->geom.pos[2] / tile_size + 0.5);

        const ai::DynamicScene::SoilProps& props = ai::DynamicScene::Instance()->GetSoilProps(x, z);

        return CollideWheelSurface(wheel, props.m_idx, props.m_friction, contact, count, reverse);
    };

    void Apply() {
        routines::Redirect(0x0729, (void*)0x00891680, &CollideWheelAndAsphalt);
        routines::Redirect(0x0971, (void*)0x00891DB0, &CollideWheelAndLandscape);
    };
};