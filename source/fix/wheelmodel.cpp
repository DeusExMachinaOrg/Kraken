#define LOGGER "wheelmodel"

#include "ext/logger.hpp"
#include "config.hpp"
#include "fix/wheelmodel.hpp"
#include "fix/wheelmodel_core.hpp"

#include "hta/CVector.hpp"
#include "hta/ai/Wheel.hpp"
#include "hta/ai/Vehicle.hpp"
#include "ode/ode.hpp"

#include <cmath>

namespace kraken::fix::wheelmodel {

    // ---- ODE Hinge2 accessors (the wheel joint gives the live, steer-aware frame) ----
    // __fastcall: joint in ecx, out-vector in edx (writes float[4]); rate returns float.
    using GetVec3Fn = void (__fastcall*)(void* joint, float* out3);
    using GetRateFn = float(__fastcall*)(void* joint);
    static const auto dJointGetHinge2Axis1      = reinterpret_cast<GetVec3Fn>(0x007d0040); // suspension/up û
    static const auto dJointGetHinge2Axis2      = reinterpret_cast<GetVec3Fn>(0x007d00f0); // axle â
    static const auto dJointGetHinge2Angle2Rate = reinterpret_cast<GetRateFn>(0x007d0300); // wheel spin ω

    // gravity is along −Y (dWorldSetGravity 0,−g,0) ⇒ world up = +Y.
    static const vec3 WORLD_UP{ 0.0f, 1.0f, 0.0f };

    static inline vec3 toVec3(const hta::CVector& v) { return { v.x, v.y, v.z }; }

    // Latest physics substep, pushed from physic.cpp::dInternalStepIsland_x2.
    // Seeded with HTA's fixed 1/120 s substep so the model is sane before the
    // first step hook fires.
    static float s_stepSize = 1.0f / 120.0f;

    void  SetStepSize(float stepsize) { if (stepsize > 1e-6f) s_stepSize = stepsize; }
    float GetStepSize(void)           { return s_stepSize; }

    WMParams ParamsFromConfig(void) {
        const kraken::Config& c = kraken::Config::Instance();
        WMParams p;
        p.k_t         = c.wheelmodel_tyre_stiffness.value;
        p.zeta_t      = c.wheelmodel_tyre_damping.value;
        p.lambda      = c.wheelmodel_hard_core_lambda.value;
        p.mu          = c.wheelmodel_grip.value;
        p.B           = c.wheelmodel_pac_B.value;
        p.C           = c.wheelmodel_pac_C.value;
        p.E           = c.wheelmodel_pac_E.value;
        p.eps         = c.wheelmodel_slip_floor.value;
        p.stick_speed = c.wheelmodel_stick_speed.value;
        p.inertia     = c.wheelmodel_wheel_inertia.value;
        return p;
    }

    // -------------------------------------------------------------------------
    //  SelfTest — validates the core routine against wheel_model.md §6 on
    //  synthetic inputs, independent of the engine. Logged at Apply().
    // -------------------------------------------------------------------------
    bool SelfTest(void) {
        WMParams P;                 // defaults
        const float dt = 1.0f / 120.0f;
        const float R = 0.5f, tau = 0.1f, m = 500.0f;
        bool ok = true;

        auto approxZero = [](float v, float tol) { return fabsf(v) < tol; };

        // Case 1 — flat ground settles level: n̂=û, at rest ⇒ vertical normal
        // force only, no longitudinal / lateral force, no spin reaction.
        {
            const vec3 c{ 0, 0, 0 }, u{ 0, 1, 0 }, a{ 1, 0, 0 }, n{ 0, 1, 0 };
            const vec3 p{ 0, -R, 0 };
            const float pen = 0.02f; // < tau ⇒ F_n = k_t·pen
            WMForce f = GeneralizedContactForce(p, n, pen, 1.0f, c, a, vec3{}, 0.0f, R, tau, m, dt, P);
            const float expectedFn = P.k_t * pen;
            const bool pass = f.F.y > 0.0f
                && approxZero(f.F.x, 1.0f) && approxZero(f.F.z, 1.0f)
                && approxZero(f.fpar_w, 1e-3f)
                && fabsf(f.F.y - expectedFn) < 1.0f;
            LOG_INFO("SelfTest[1] flat ground: F=(%.1f, %.1f, %.1f) fpar=%.4f expFn=%.1f -> %s",
                     f.F.x, f.F.y, f.F.z, f.fpar_w, expectedFn, pass ? "PASS" : "FAIL");
            ok = ok && pass;
        }

        // Case 2 — degenerate side (n̂ ∥ â): rolling tangent collapses ⇒ normal
        // push only (no friction, no spin reaction).
        {
            const vec3 c{ 0, 0, 0 }, a{ 1, 0, 0 }, n{ 1, 0, 0 };
            const vec3 p{ 0.45f, 0, 0 };
            const float pen = 0.05f; // δ_s
            WMForce f = GeneralizedContactForce(p, n, pen, 1.0f, c, a, vec3{}, 0.0f, R, tau, m, dt, P);
            const bool pass = f.F.x > 0.0f
                && approxZero(f.F.y, 1e-3f) && approxZero(f.F.z, 1e-3f)
                && approxZero(f.fpar_w, 1e-6f);
            LOG_INFO("SelfTest[2] side/disc: F=(%.1f, %.1f, %.1f) fpar=%.6f -> %s",
                     f.F.x, f.F.y, f.F.z, f.fpar_w, pass ? "PASS" : "FAIL");
            ok = ok && pass;
        }

        // Case 3 — wall/step: n̂ horizontal ⇒ rolling tangent t̂=â×n̂ is VERTICAL,
        // and a spinning wheel produces a vertical traction force (climb).
        {
            const vec3 c{ 0, 0, 0 }, u{ 0, 1, 0 }, a{ 1, 0, 0 }, n{ 0, 0, 1 };
            const vec3 p{ 0, 0, 0.5f };
            const vec3 tHat = Normalized(Cross(a, n)); // recomputed to assert verticality
            const float omega = 40.0f;                 // spinning hard
            const float pen = 0.03f;
            WMForce f = GeneralizedContactForce(p, n, pen, 1.0f, c, a, vec3{}, omega, R, tau, m, dt, P);
            const bool verticalTangent = fabsf(tHat.y) > 0.99f;
            const bool climbs = fabsf(f.fpar_w) > 1e-3f && fabsf(f.F.y) > 1.0f;
            const bool pass = verticalTangent && climbs;
            LOG_INFO("SelfTest[3] wall climb: tHat=(%.2f, %.2f, %.2f) F=(%.1f, %.1f, %.1f) fpar=%.2f -> %s",
                     tHat.x, tHat.y, tHat.z, f.F.x, f.F.y, f.F.z, f.fpar_w, pass ? "PASS" : "FAIL");
            ok = ok && pass;
        }

        // Case 4 — §2 classify/aggregate: a ground + a wall contact must resolve
        // to distinct slots (star ground vs dagger obstacle). Contacts sit inside
        // R (penetrating). Exercises ComputeGeom + Classify used by Stage 1+.
        {
            WMContact cts[2];
            cts[0].p = vec3{ 0, -R, 0 };  cts[0].n = vec3{ 0, 1, 0 }; cts[0].depth = 0.02f; // ground
            cts[1].p = vec3{ 0, 0, R };   cts[1].n = vec3{ 0, 0, 1 }; cts[1].depth = 0.03f; // wall
            const vec3 c{ 0, 0, 0 }, u{ 0, 1, 0 }, a{ 1, 0, 0 };
            WMGeom gm[2];
            for (int i = 0; i < 2; ++i) gm[i] = ComputeGeom(cts[i], c, u, a, R, 0.15f);
            WMSlots s = Classify(gm, 2);
            const bool pass = s.ground == 0 && s.obstacle == 1;
            LOG_INFO("SelfTest[4] classify: ground=%d obstacle=%d side=%d (pG=%.3f pO=%.3f) -> %s",
                     s.ground, s.obstacle, s.side, gm[0].pen, gm[1].pen, pass ? "PASS" : "FAIL");
            ok = ok && pass;
        }

        LOG_INFO("SelfTest overall: %s", ok ? "PASS" : "FAIL");
        return ok;
    }

    // -------------------------------------------------------------------------
    //  OnWheelContacts — reconstructs the wheel frame from the Hinge2 joint, runs
    //  §2 classify + §3 force on the real ODE manifold. With [wheelmodel] apply=0
    //  it only logs (read-only); with apply=1 it pushes the penalty force onto the
    //  wheel body and zeros *numContacts so ODE does not also resolve the contact.
    // -------------------------------------------------------------------------
    static constexpr int MAX_CONTACTS = 16;

    void OnWheelContacts(hta::ai::Wheel* wheel, dContact* contacts, unsigned int* numContacts) {
        const kraken::Config& config = kraken::Config::Instance();
        const unsigned int count = numContacts ? *numContacts : 0;
        if (!config.wheelmodel_enabled.value || !wheel || !contacts || count == 0)
            return;

        void* joint = wheel->m_jointID;
        if (!joint) return; // detached/broken wheel — leave to stock path

        hta::ai::Vehicle* vehicle = wheel->GetVehicle();
        if (!vehicle) return;

        // Apply only to the player's vehicle while stabilising (player_only=1), so a
        // model glitch cannot cascade through the AI cars sharing the physics island.
        const bool applyThis = config.wheelmodel_apply.value != 0 &&
            (!config.wheelmodel_player_only.value || vehicle->m_bIsControlledByPlayer);

        // --- live frame from the Hinge2 joint ---
        float ax1[4] = { 0,0,0,0 }, ax2[4] = { 0,0,0,0 };
        dJointGetHinge2Axis1(joint, ax1); // suspension/up
        dJointGetHinge2Axis2(joint, ax2); // axle
        vec3 u = Normalized(vec3{ ax1[0], ax1[1], ax1[2] });
        vec3 a = Normalized(vec3{ ax2[0], ax2[1], ax2[2] });
        if (Dot(u, WORLD_UP) < 0.0f) u = u * -1.0f; // orient û up
        const vec3 f = Normalized(Cross(a, u));     // rolling forward (sign TBD in-sim)

        const vec3  c        = toVec3(wheel->GetPosition());
        // WORLD centre of mass (dBodyGetPosition). GetMassCenter() returns only the
        // LOCAL offset (~0), which made p−comW ≈ the full world position and turned
        // tiny angular velocity into hundreds of m/s of bogus contact velocity.
        const vec3  comW     = toVec3(vehicle->GetMassCenterPosition());
        const vec3  vLin     = toVec3(vehicle->GetLinearVelocity());
        const vec3  vAng     = toVec3(vehicle->GetAngularVelocity());
        // Spin ω about the axle: the wheel body's angular velocity projected on â
        // (dJointGetHinge2Angle2Rate reads ~0 in-sim). v_p below is the chassis
        // carrier velocity (no spin); §3 re-adds (ωâ)×r for the contact velocity.
        const float omega    = Dot(toVec3(wheel->GetAngularVelocity()), a);
        const float omegaEng = dJointGetHinge2Angle2Rate(joint); // diagnostic only
        const float R        = wheel->GetRadius();
        const float H        = wheel->GetWidth() * 0.5f;
        const uint32_t nWhl  = vehicle->GetNumWheels();
        const float m        = (nWhl > 0) ? vehicle->GetMass() / (float)nWhl : vehicle->GetMass();
        const float tau      = fminf(config.wheelmodel_tyre_thickness.value, R * 0.9f);
        const float dt       = GetStepSize();
        const WMParams P     = ParamsFromConfig();

        // Safety rails (a stiff penalty spring + save-load transients can spike).
        const float gAbs      = fmaxf(fabsf(config.gravity.value), 0.1f);
        const float maxForce  = config.wheelmodel_max_g.value * m * gAbs; // per-contact cap
        const float maxSpeed  = config.wheelmodel_max_speed.value;
        const float healthyFn = 0.25f * m * gAbs; // suppress ODE only above this support
        const float reactScale = config.wheelmodel_react_scale.value; // spin↔traction coupling

        const unsigned int n = (count < (unsigned)MAX_CONTACTS) ? count : (unsigned)MAX_CONTACTS;
        WMContact cts[MAX_CONTACTS];
        WMGeom    gm[MAX_CONTACTS];
        for (unsigned int i = 0; i < n; ++i) {
            const dContactGeom& g = contacts[i].geom;
            cts[i].p = { g.pos[0], g.pos[1], g.pos[2] };
            cts[i].n = { g.normal[0], g.normal[1], g.normal[2] };
            cts[i].depth = g.depth;
            gm[i] = ComputeGeom(cts[i], c, u, a, R, H);
        }
        WMSlots slots = Classify(gm, (int)n);

        auto vpAt = [&](const vec3& p) {
            return vLin + Cross(vAng, p - comW); // chassis carrier velocity (no spin)
        };
        auto finite = [](float v) { return v == v && v < 1e18f && v > -1e18f; };

        // Compute the §3 force for a slot and, when applying, push it onto the
        // CHASSIS at the contact point (concept: apply at the attachment point on
        // the chassis). Chassis application is stable regardless of the unsprung
        // wheel-body mass and self-regulates to depth_eq = mg/k_t; vertical friction
        // still lifts it (climb). Guarded: NaN-drop, per-contact cap, skip if the
        // body is already flying. The friction's spin reaction goes to the wheel body.
        bool healthy = false;    // did the radial (support) force reach a real value?
        bool bodyFlying = false; // a contact point exceeded maxSpeed (transient/blow-up)
        auto processSlot = [&](int idx, bool side) -> WMForce {
            WMForce fr;
            if (idx < 0) return fr;
            const WMGeom& g = gm[idx];
            const float w  = side ? g.wl : g.wr;
            const vec3  p  = cts[idx].p;
            const vec3  vp = vpAt(p);
            if (Len(vp) > maxSpeed) { bodyFlying = true; return fr; } // don't feed a flying body
            fr = GeneralizedContactForce(p, cts[idx].n, g.pen, w, c, a, vp, omega,
                                         R, tau, m, dt, P);
            float fmag = Len(fr.F);
            if (!(finite(fr.F.x) && finite(fr.F.y) && finite(fr.F.z))) { fr = WMForce(); fmag = 0.0f; }
            else if (fmag > maxForce && fmag > 1e-6f) { fr.F = fr.F * (maxForce / fmag); } // cap
            if (!side && Dot(fr.F, u) >= healthyFn) healthy = true;
            if (applyThis && fmag > 0.0f) {
                vehicle->AddForceAtPos(hta::CVector(fr.F.x, fr.F.y, fr.F.z),
                                       hta::CVector(p.x, p.y, p.z));
                // Stage 3 — spin↔traction coupling. The friction (tangential) part of
                // the contact force, acting at the contact point, torques the wheel
                // about its axle. Feed that spin torque to the wheel body so traction
                // couples to spin (wheelspin, grip-limited accel, spin-driven climb);
                // the drivetrain still drives the wheel and _CalcRpms still reads it,
                // so RPM stays correct. Normal force is ~radial ⇒ ~no spin torque.
                if (!side && reactScale > 0.0f) {
                    const vec3  Ft    = fr.F - cts[idx].n * Dot(fr.F, cts[idx].n); // friction only
                    const float tSpin = Dot(Cross(p - c, Ft), a) * reactScale;     // about the axle
                    wheel->AddTorque(hta::CVector(a.x * tSpin, a.y * tSpin, a.z * tSpin));
                }
            }
            return fr;
        };

        const WMForce fG = processSlot(slots.ground,   false);
        const WMForce fO = processSlot(slots.obstacle, false);
        const WMForce fS = processSlot(slots.side,     true);

        // Commit: suppress ODE's wheel contact whenever we're actively supporting
        // this wheel, so it penetrates and our tyre spring builds real support
        // (settling to depth_eq = mg/k_t). We back off only for a flying body
        // (velocity guard) — not for low force — so it does NOT deadlock the way a
        // "suppress only when healthy" gate did (ODE would hold pen≈0 forever).
        if (applyThis && !bodyFlying && numContacts) *numContacts = 0;

        if (!config.wheelmodel_log.value) return;
        if (!vehicle->m_bIsControlledByPlayer) return;
        // Dense window right after start (catch load transients), then throttle.
        static uint32_t s_calls = 0;
        const bool verbose = s_calls < 400;
        ++s_calls;
        if (!verbose && (s_calls & 31) != 0) return;

        auto logSlot = [&](const char* tag, int idx, bool side, const WMForce& fr) {
            if (idx < 0) return;
            const WMGeom& g = gm[idx];
            const vec3 vp = vpAt(cts[idx].p);
            LOG_INFO("  %-8s nu=%.2f na=%.2f pen=%.3f w=%.2f vn=%.1f |vp|=%.1f "
                     "F=(%.0f,%.0f,%.0f) |F|=%.0f fpar=%.0f",
                     tag, Dot(cts[idx].n, u), Dot(cts[idx].n, a), g.pen, side ? g.wl : g.wr,
                     Dot(vp, cts[idx].n), Len(vp), fr.F.x, fr.F.y, fr.F.z, Len(fr.F), fr.fpar_w);
        };

        LOG_INFO("wheel=%p veh=%p nC=%u applyThis=%d healthy=%d dt=%.4f R=%.2f m=%.0f maxF=%.0f "
                 "u=(%.2f,%.2f,%.2f) a=(%.2f,%.2f,%.2f) f=(%.2f,%.2f,%.2f) |vLin|=%.1f om=%.2f omEng=%.2f",
                 (void*)wheel, (void*)vehicle, n, applyThis ? 1 : 0, healthy ? 1 : 0, dt, R, m, maxForce,
                 u.x, u.y, u.z, a.x, a.y, a.z, f.x, f.y, f.z, Len(vLin), omega, omegaEng);
        logSlot("ground",   slots.ground,   false, fG);
        logSlot("obstacle", slots.obstacle, false, fO);
        logSlot("side",     slots.side,     true,  fS);
    }

    void Apply(void) {
        const kraken::Config& config = kraken::Config::Instance();
        if (!config.wheelmodel_enabled.value) {
            return; // stock HTA dynamics; nothing installed
        }

        LOG_INFO("Wheel model v3 enabled (apply=%u log=%u). k_t=%.0f grip=%.2f tau=%.3f",
                 config.wheelmodel_apply.value, config.wheelmodel_log.value,
                 config.wheelmodel_tyre_stiffness.value, config.wheelmodel_grip.value,
                 config.wheelmodel_tyre_thickness.value);
        SelfTest();

        // The contact plumbing / force application live in OnWheelContacts, driven
        // by the shared wheel-collide hook (cardan.cpp) — no redirect installed
        // here; everything is behind the [wheelmodel] enabled/apply gates.
    }

} // namespace kraken::fix::wheelmodel
