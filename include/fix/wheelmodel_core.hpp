#ifndef KRAKEN_FIX_WHEELMODEL_CORE
#define KRAKEN_FIX_WHEELMODEL_CORE

// =============================================================================
//  Single-Wheel Contact Model v3 — engine-agnostic force routine ("the artefact")
//
//  Pure math port of wheel_model.md §2 (classify/aggregate), §3 (generalised
//  contact force: normal spring along n̂ + Pacejka friction along the rolling
//  tangent t̂ = â×n̂) and §5 (spin DOF). No engine types — plain float/vec3 — so
//  it can be unit-tested (see wheelmodel::SelfTest) and later dropped onto any
//  contact manifold. The HTA glue in wheelmodel.cpp feeds it ODE contacts.
//
//  NOTE on the weight w: the doc's boxed final force scales the whole contact by
//  w (F = (F_n n̂ + f∥ t̂ + f_ℓ ℓ̂)·w) and the returned longitudinal is f∥·w. To
//  keep the friction circle equal to μ·(applied normal) after that outer ·w, the
//  friction limit D uses the UNWEIGHTED normal N = F_n (the "N = F_n w" in the
//  text folds the single outer w — applying w to both would give w²). On flat
//  ground w≈1 so this only matters for oblique contacts.
// =============================================================================

#include <cmath>

namespace kraken::fix::wheelmodel {

    struct vec3 {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        vec3() = default;
        vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    };

    inline vec3   operator+(const vec3& a, const vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline vec3   operator-(const vec3& a, const vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    inline vec3   operator*(const vec3& a, float s)       { return { a.x * s, a.y * s, a.z * s }; }
    inline float  Dot(const vec3& a, const vec3& b)       { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline vec3   Cross(const vec3& a, const vec3& b)     { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    inline float  Len(const vec3& a)                      { return sqrtf(Dot(a, a)); }
    inline vec3   Normalized(const vec3& a) {
        float l = Len(a);
        return (l > 1e-8f) ? vec3{ a.x / l, a.y / l, a.z / l } : vec3{ 0.0f, 0.0f, 0.0f };
    }

    inline float wm_clamp(float v, float lo, float hi) { return fmaxf(lo, fminf(hi, v)); }
    inline float wm_lerp(float a, float b, float t)    { return a + (b - a) * t; }

    // -------------------------------------------------------------------------
    // Parameters (§0). k_c = lambda * k_t is the hard-core overload backstop.
    // -------------------------------------------------------------------------
    struct WMParams {
        float k_t         = 120000.0f; // tyre stiffness N/m (contact spring)
        float zeta_t      = 0.5f;      // tyre damping ratio (primary)
        float lambda      = 20.0f;     // hard-core factor: k_c = lambda * k_t
        float mu          = 1.5f;      // Pacejka peak grip
        float B           = 8.0f;      // Pacejka stiffness factor
        float C           = 1.5f;      // Pacejka shape C
        float E           = 0.97f;     // Pacejka shape E
        float eps         = 0.5f;      // slip floor (v_ref minimum)
        float stick_speed = 0.5f;      // v0: static-friction blend speed
        float inertia     = 5.0f;      // wheel spin inertia I
    };

    // Pacejka magic formula Φ(ξ) = D·sin(C·atan(Bξ − E(Bξ − atan Bξ))).
    inline float Phi(float xi, float D, const WMParams& P) {
        const float Bx = P.B * xi;
        const float inner = Bx - P.E * (Bx - atanf(Bx));
        return D * sinf(P.C * atanf(inner));
    }

    // -------------------------------------------------------------------------
    // §1 geometry for one raw contact, resolved against the idealised wheel
    // cylinder (center c, up û, axle â, radius R, half-width H).
    // -------------------------------------------------------------------------
    struct WMContact {
        vec3  p;             // contact point (world)
        vec3  n;             // surface normal (world, unit, out of the surface)
        float depth = 0.0f;  // ODE-reported penetration (reference / logging only)
    };

    struct WMGeom {
        float s      = 0.0f; // â·r (along-axle component, diagnostic)
        float d      = 0.0f; // |r⊥| (off-axle distance, diagnostic)
        float pen    = 0.0f; // penetration used by the spring (ODE manifold depth)
        float chi    = 0.0f; // |n̂·â|
        float wr     = 1.0f; // radial weight 1 − χ
        float wl     = 0.0f; // side weight   χ
        bool  ground = true; // n̂·û > ½  (else obstacle: wall/step)
    };

    // Penetration comes from the engine's contact manifold (ct.depth = ODE depth),
    // NOT the ShapeCast reconstruction R−d / H−|s|: HTA wheels are spheres, so the
    // contact point sits on the surface (d≈R ⇒ R−d≈0) and |s|≈0 makes H−|s|
    // spuriously large — exactly the §1 scaffolding "a real ContactManifold
    // replaces." Classification (ground/obstacle/side) is by the normal only.
    inline WMGeom ComputeGeom(const WMContact& ct, const vec3& c,
                              const vec3& u, const vec3& a, float /*R*/, float /*H*/) {
        WMGeom g;
        const vec3 r = ct.p - c;
        g.s = Dot(a, r);
        g.d = Len(r - a * g.s);
        g.pen = fmaxf(ct.depth, 0.0f);
        g.chi = fabsf(Dot(ct.n, a));
        g.wr = 1.0f - g.chi;
        g.wl = g.chi;
        g.ground = Dot(ct.n, u) > 0.5f;
        return g;
    }

    // -------------------------------------------------------------------------
    // §2 aggregate: keep the deepest ground (★), obstacle (†) and side (‡).
    // Splitting by normal avoids the v1 "super-wheel" and the v2 wall-vs-ground
    // slot fight. Indices are into the parallel contact/geom arrays (−1 = none).
    // -------------------------------------------------------------------------
    struct WMSlots { int ground = -1, obstacle = -1, side = -1; };

    inline WMSlots Classify(const WMGeom* gm, int n) {
        WMSlots s;
        float bestG = 0.0f, bestO = 0.0f, bestS = 0.0f;
        for (int i = 0; i < n; ++i) {
            const WMGeom& g = gm[i];
            if (g.pen <= 0.0f) continue;
            const float rScore = g.wr * g.pen; // radial (ground/obstacle)
            const float sScore = g.wl * g.pen; // side — only wins for axial normals (χ high)
            if (g.ground) { if (rScore > bestG) { bestG = rScore; s.ground = i; } }
            else          { if (rScore > bestO) { bestO = rScore; s.obstacle = i; } }
            if (sScore > bestS) { bestS = sScore; s.side = i; }
        }
        return s;
    }

    // -------------------------------------------------------------------------
    // §3 generalised contact force. One routine for ground / obstacle / side.
    // pen = δ (radial) for ★/†, or δ_s for ‡. Returns the world force and the
    // longitudinal share f∥·w that feeds the spin reaction (§5).
    // -------------------------------------------------------------------------
    struct WMForce { vec3 F; float fpar_w = 0.0f; };

    inline WMForce GeneralizedContactForce(
        const vec3& p, const vec3& n, float pen, float w,
        const vec3& c, const vec3& a,
        const vec3& v_p, float omega,
        float R, float tau, float m, float dt,
        const WMParams& P)
    {
        WMForce out;
        if (pen <= 0.0f || w <= 0.0f) return out;

        const vec3 r = p - c;

        // --- Normal (tyre spring along n̂ — roll-stable) ---
        const float delta_soft = fminf(pen, tau);
        const float delta_hard = fmaxf(0.0f, pen - tau);
        // Damping is active whenever the tyre is in contact (τ>0). The concept ramps
        // it by δ/τ, but that assumes τ ≈ the resting penetration mg/k_t. When τ is a
        // soft band ≫ resting penetration, that ramp leaves the spring almost
        // undamped at equilibrium ⇒ it rings ("bounces in place"). Full contact
        // damping restores the intended ζ_t; τ still governs the soft/hard split.
        const float g = (tau > 0.0f) ? 1.0f : 0.0f;
        const float k_c = P.lambda * P.k_t;
        const float v_n = Dot(v_p, n);
        const float c_t = 2.0f * P.zeta_t * sqrtf(fmaxf(P.k_t * m, 0.0f));
        const float F_n = fmaxf(0.0f, P.k_t * delta_soft + k_c * delta_hard - c_t * g * v_n);

        vec3 Fvec = n * F_n; // normal component (pre-weight)

        // --- Friction (Pacejka along the rolling tangent) ---
        const vec3 rawT = Cross(a, n);
        const float tLen = Len(rawT);
        if (tLen > 1e-4f && F_n > 0.0f && dt > 0.0f) {
            const vec3 t = rawT * (1.0f / tLen);   // rolling tangent
            const vec3 l = Cross(n, t);            // lateral

            const vec3 v_c = v_p + Cross(a * omega, r); // + wheel spin
            const float v_par = Dot(v_c, t);
            const float v_lat = Dot(v_c, l);
            const float v_ref = fmaxf(fabsf(Dot(v_p, t)), P.eps);

            const float kappa = -v_par / v_ref;
            const float alpha = atan2f(-v_lat, v_ref);

            const float D = P.mu * F_n; // unweighted (see header note)
            float f_par = Phi(kappa, D, P);
            float f_lat = Phi(alpha, D, P);

            // friction circle
            const float mag = sqrtf(f_par * f_par + f_lat * f_lat);
            if (mag > D && mag > 1e-6f) {
                const float sc = D / mag;
                f_par *= sc;
                f_lat *= sc;
            }

            // no-overshoot clamps
            const float capPar = (R > 1e-4f) ? P.inertia * fabsf(v_par) / (R * R * dt) : 0.0f;
            f_par = wm_clamp(f_par, -capPar, capPar);
            const float capLat = m * fabsf(v_lat) / dt;
            f_lat = wm_clamp(f_lat, -capLat, capLat);

            // static friction near standstill: blend lateral to a critical damper
            // that holds the patch (the slip curve mis-fires at ~0 speed).
            const float stick = wm_clamp(1.0f - fabsf(Dot(v_p, t)) / P.stick_speed, 0.0f, 1.0f);
            const float damper = wm_clamp(-v_lat * 2.0f * sqrtf(fmaxf(P.k_t * m, 0.0f)), -D, D);
            f_lat = wm_lerp(f_lat, damper, stick);

            Fvec = Fvec + t * f_par + l * f_lat;
            out.fpar_w = f_par * w;
        }

        out.F = Fvec * w;
        return out;
    }

    // §5 spin integrator. airborne: no reaction torque (τ_react folded out).
    inline float IntegrateSpin(float omega, float tau_drive, float tau_react,
                               float tau_brake, float I, float dt) {
        if (I <= 1e-6f) return omega;
        return omega + (tau_drive - tau_react - tau_brake) / I * dt;
    }

} // namespace kraken::fix::wheelmodel

#endif
