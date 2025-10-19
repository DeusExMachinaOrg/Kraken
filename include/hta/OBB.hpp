#pragma once

#include "hta/CMatrix.hpp"
#include "hta/CVector.h"
#include "hta/AABB.hpp"
#include <stdint.h>

class Obb { /* Size=0x48 */
  /* 0x0000 */ public: CVector m_origin;
  /* 0x000c */ public: CVector m_basis[3];
  /* 0x0030 */ public: CVector m_min;
  /* 0x003c */ public: CVector m_max;
  
  public: CVector toWorld(const CVector&) const;
  public: CVector toLocalRotate(const CVector&) const;
  public: void Create(const CVector&, const CVector&, const CMatrix&, bool);
  public: void Create(const Aabb&, const CMatrix&, bool);
  public: void Draw(uint32_t);
  public: float IntersectRay(const CVector&, const CVector&) const;
  public: int32_t IsPtInside(const CVector&) const;
  public: int32_t IsPtInside2(const CVector&) const;
  public: Aabb GetBounds() const;
  public: Obb(const Obb&);
  public: Obb();
};