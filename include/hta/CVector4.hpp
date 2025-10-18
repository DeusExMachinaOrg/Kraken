#pragma once

struct CVector;
struct CVector4 { /* Size=0x10 */
  /* 0x0000 */ public: float x;
  /* 0x0004 */ public: float y;
  /* 0x0008 */ public: float z;
  /* 0x000c */ public: float w;
  
  public: CVector4(const CVector&, float);
  public: CVector4(float, float, float, float);
  public: CVector4();
};
