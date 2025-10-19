#pragma once

struct CVector2 { /* Size=0x8 */
  /* 0x0000 */ public: float x;
  /* 0x0004 */ public: float y;
  
  CVector2(const CVector2&);
  CVector2(float, float);
  CVector2();
  void zero();
  void one();
  CVector2 normalize() const;
  operator float *() const;
  operator float *();
  float length() const;
  float lengthSq() const;
  CVector2& operator-=(const CVector2&);
  CVector2& operator+=(const CVector2&);
  CVector2& operator*=(float);
  CVector2& operator/=(float);
  float randomValue() const;
};