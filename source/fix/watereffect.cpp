#define LOGGER "watereffect"

#include "ext/logger.hpp"
#include "routines.hpp"
#include "config.hpp"

#include "hta/m3d/Object.h"
#include "hta/ai/PhysicObj.h"
#include "ode/ode.hpp"

#include "fix/watereffect.hpp"

namespace kraken::fix::watereffect {
    __declspec(naked) void Hook_CollidePOAndWater()
    {
        __asm {
            // Вход: ecx = obj1, edx = obj2, стек: [esp] = ret, [esp+4]=contacts, [esp+8]=numContacts, [esp+0Ch]=reverse
            push    ebp
            mov     ebp, esp

            mov     eax, [ebp + 0Ch]    // reverse
            push    eax

            mov     eax, [ebp + 8]      // numContacts
            push    eax

            mov     eax, [ebp + 4]      // contacts
            push    eax

            push    edx                 // obj2
            push    ecx                 // obj1

            call    CollidePOAndWater

            mov     esp, ebp
            pop     ebp
            ret     0Ch
        }
    }

    int __fastcall CollidePOAndWater(
    m3d::Object* obj1, // ecx
    m3d::Object* obj2, // edx
    dContact* contacts,
    unsigned int* numContacts,
    bool reverse)
    {
        if (obj1->IsKindOf((m3d::Class*)0x00A00B6C)) {
            CVector vec = ((ai::PhysicObj*)obj1)->GetLinearVelocity();
            if (sqrtf(vec.x * vec.x + vec.y * vec.y + vec.z * vec.z) >= 1.0f) {
                CVector pos(contacts->geom.pos[0], contacts->geom.pos[1], contacts->geom.pos[2]);
                
            }
        }
        return 0;
    }

    void Apply()
    {
        kraken::routines::Redirect(0xC0, (void*) 0x00890DD0, (void*) &CollidePOAndWater);
    }
}