#ifndef KRAKEN_FIX_WHEELMODEL
#define KRAKEN_FIX_WHEELMODEL

// Single-Wheel Contact Model v3 (see wheel_model.md / plan). Penalty-force wheel
// model. Gated by [wheelmodel] enabled (default 0 = stock HTA). The engine-agnostic
// force routine lives in wheelmodel_core.hpp; this is the HTA glue + wiring.

struct dContact;
namespace hta::ai { struct Wheel; }

namespace kraken::fix::wheelmodel {
    void Apply(void);

    // Latest physics substep Δt, pushed from physic.cpp::dInternalStepIsland_x2.
    void  SetStepSize(float stepsize);
    float GetStepSize(void);

    // Self-check of the core routine against wheel_model.md §6 (logged at Apply).
    bool SelfTest(void);

    // Fed the ODE contact manifold for one (wheel, object) pair from the shared
    // wheel-collide hook (cardan.cpp::Hook_CollideWheelDefault). With apply off it
    // only classifies + logs (read-only). With [wheelmodel] apply=1 it applies the
    // §3 penalty force to the wheel body and zeros *numContacts so ODE does not
    // also resolve the contact (Stage 2). numContacts may be null.
    void OnWheelContacts(hta::ai::Wheel* wheel, dContact* contacts, unsigned int* numContacts);
}

#endif
