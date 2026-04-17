#define LOGGER "collidefix"

#include "fix/watercollidefix.hpp"

#include "ext/logger.hpp"
#include "routines.hpp"

#include "hta/CVector.hpp"
#include "hta/ai/PhysicObj.hpp"
#include "hta/ai/PhysicBody.hpp"

using namespace hta;

namespace kraken::fix::collidefix {
    CUSTOM static bool CanCreateCollisionEffect2(const hta::ai::PhysicObj* obj) {
        if (!obj) {
            return false;
        }

        return obj->m_timeFromLastCollisionEffect > 0.1f;
    }

    CUSTOM static int32_t __fastcall CollidePOAndWater(hta::m3d::Object* obj1, hta::m3d::Object* obj2, dContact* contacts, uint32_t& numContacts, bool reverse) {
        if (!obj1 || !obj2 || !contacts) {
            return 0;
        }

        hta::ai::PhysicObj* physicObj = nullptr;
        if (obj1->GetClass() && obj1->GetClass()->IsKindOf(hta::ai::PhysicObj::GetBaseClass())) {
            physicObj = static_cast<hta::ai::PhysicObj*>(obj1);
        } else if (obj2->GetClass() && obj2->GetClass()->IsKindOf(hta::ai::PhysicObj::GetBaseClass())) {
            physicObj = static_cast<hta::ai::PhysicObj*>(obj2);
        }

        if (!physicObj) {
            return 0;
        }

        const CVector velocity = physicObj->GetLinearVelocity();
        constexpr float kMinSplashSpeed = 1.0f;
        if (velocity.LengthSquare() < kMinSplashSpeed * kMinSplashSpeed) {
            return 0;
        }

        if (!CanCreateCollisionEffect2(physicObj)) {
            return 0;
        }

        const CVector contactPos(
            static_cast<float>(contacts->geom.pos[0]),
            static_cast<float>(contacts->geom.pos[1]),
            static_cast<float>(contacts->geom.pos[2]));

        Quaternion splashRotation;
        splashRotation.Identity();
        const CStr splashName("ET_PS_SPLINTER_WATERSPLASH");
        hta::ai::PhysicBody::CreateEffectNode(splashName, contactPos, splashRotation, true, 1.0f);
        physicObj->SetCollisionEffectCreated();

        return 0;
    }

    void Apply() {
        LOG_INFO("Feature enabled");
        kraken::routines::Redirect(0xC0, (void*)0x00890DD0, (void*)&CollidePOAndWater);
    }
}