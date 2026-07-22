#ifndef KRAKEN_FIX_KINETICFRICTION
#define KRAKEN_FIX_KINETICFRICTION

#include <cmath>

namespace kraken::fix::kineticfriction {
    void Apply(void);

    // Shared with fix::joltshadow (docs §23.8): the same slip-based tire friction curve used
    // for ODE-driven vehicles (via CollideWheelAndAsphalt/CollideWheelAndLandscape below) is
    // reused, via JPH::WheeledVehicleController::SetTireMaxImpulseCallback, for Jolt-driven
    // ones - so a shadowed vehicle's tire feel isn't a different (Jolt's own simplified linear
    // curve), unrelated model from every other vehicle in the game. Pulled out of
    // kineticfriction.cpp into this header (previously .cpp-local) purely so both translation
    // units can call the exact same formula/constants instead of drifting apart over time.
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
    }

    inline float mu_from_kappa(float kappa, const TireParams& t) {
        const float x = fabsf(kappa);
        const float G = 1.0f - expf(-powf(fmaxf(x, 1e-6f) / t.xg, t.q));
        const float D = 1.0f / (1.0f + powf(x / t.xd, t.p));
        float mu = t.mu_min + (t.mu_peak - t.mu_min) * (G * D);
        const float w0 = 1.0f - expf(-powf(x / t.x0, t.r0));
        mu = w0 * mu + (1.0f - w0) * t.mu_static;
        return fminf(mu, t.mu_cap);
    }
};

#endif
