#pragma once

struct dContact; // global ODE contact struct (ode/ode.hpp)

namespace hta::ai {
    struct Wheel;
}
namespace hta::m3d {
    struct Object;
}

namespace kraken::fix::thorncollide {
    void Apply();

    // Called from the shared ai::CollideWheelDefault hook (owned by fix::cardan) after the
    // stock tyre logic runs. 'other' is the engine-resolved object that struck the wheel
    // (the collider's edx argument). Applies ram damage when that object belongs to another
    // vehicle, closing the side-ram "dead zone". No-op unless [constants] appendix and
    // [thorncollide] wheel_damage are both enabled.
    void OnWheelCollision(hta::ai::Wheel* wheel, hta::m3d::Object* other, dContact* contacts, unsigned int* numContacts);
}
