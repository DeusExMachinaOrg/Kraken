#pragma once

#include "hta/OBB.hpp"
#include "hta/AABB.hpp"
#include "hta/CVector.h"
#include "hta/Quaternion.h"
#include "hta/m3d/Object.h"
#include "hta/ai/PhysicObj.h"
#include "hta/m3d/SgNode.hpp"

#include <stdint.h>

namespace ai {
    struct SphereForIntersection;
    struct Box;

    struct Obstacle {
        /* Size=0x18 */
        /* 0x0000 */ int32_t m_refCount;
        /* 0x0004 */ bool m_bIsEnabled;
        /* 0x0008 */ SphereForIntersection* m_intersectionSphere;
        /* 0x000c */ Box* m_intersectionBox;
        /* 0x0010 */ int32_t m_ownerPhysicObjId;
        /* 0x0014 */ m3d::SgNode* m_ownerSgNode;
    
        Obstacle(const Obb&);
        Obstacle(m3d::SgNode*);
        Obstacle(const PhysicObj*);
        ~Obstacle();
        const SphereForIntersection* GetSphere() const;
        const Box* GetBox() const;
        Aabb GetAabb() const;
        CVector GetPosition() const;
        Quaternion GetRotation() const;
        CVector GetLinearVelocity() const;
        float GetIntersectionRadius() const;
        void Enable();
        void Disable();
        bool bIsEnabled() const;
        PhysicObj* GetOwnerPhysicObj() const;
        m3d::Object* GetOwner() const;
        void UnlinkFromOwner();
        void RenderDebugInfo() const;
        int32_t IncRef();
        int32_t DecRef();
        int32_t GetRefCount() const;
        void _Init();
    };
};