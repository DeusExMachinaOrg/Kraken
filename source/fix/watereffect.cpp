#define LOGGER "watereffect"

#include "ext/logger.hpp"
#include "routines.hpp"
#include "config.hpp"

#include "hta/m3d/Object.h"
#include "hta/ai/PhysicObj.h"
#include "hta/ai/PhysicBody.h"
#include "hta/Quaternion.h"
#include "hta/ai/ObjContainer.hpp"
#include "hta/ai/JointedObj.hpp"
#include "ode/ode.hpp"

#include "fix/watereffect.hpp"

namespace kraken::fix::watereffect {
    int __fastcall CollidePOAndWater(
    m3d::Object* obj1, // ecx
    m3d::Object* obj2, // edx
    dContact* contacts,
    unsigned int* numContacts,
    bool reverse)
    {
        if (obj1->IsKindOf((m3d::Class*)0x00A00B6C)) {
            const CVector vec = ((ai::PhysicObj*)obj1)->GetLinearVelocity();
            if (sqrtf(vec.x * vec.x + vec.y * vec.y + vec.z * vec.z) >= 1.0f) {
                CVector pos(contacts->geom.pos[0], contacts->geom.pos[1], contacts->geom.pos[2]);
                //CStr modelname("ET_PS_SPLINTER_WATERSPLASH"); // original
                CStr modelname("ET_PS_PLASMAWATERSPLASH");
                const Quaternion* quat = &Quaternion::IdentityQuaternion_28;
                float scale = 1.0f;
                bool bInsert = true;
                int a9 = 0;
                bool insertInGraph = true;

                m3d::SgNode* node;

                __asm {
                    // ECX = modelname, EDX = pos
                    lea     ecx, modelname
                    lea     edx, pos

                    // Параметры на стек (userpurge)
                    push    scale             // float scale
                    push    bInsert           // bool bInsertInRemoveIfFree
                    push    quat              // const Quaternion* rot

                    // Вызов оригинальной функции
                    mov     eax, 0x00617450
                    call    eax        // адрес CreateEffectNode

                    // Сохраняем результат
                    mov     node, eax
                }

            }
        }
        return 0;
    }

    void Apply()
    {
        kraken::routines::Redirect(0xC0, (void*) 0x00890DD0, (void*) &CollidePOAndWater);
    }
}