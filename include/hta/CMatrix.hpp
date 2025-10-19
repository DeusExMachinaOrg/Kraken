//
// Information extracted with resym v0.4.0
//
// PDB file: C:\Program Files (x86)\Steam\steamapps\common\Hard Truck Apocalypse\game.pdb
// Image architecture: X86
//
#pragma once
#include <cstdint>
#include <array>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct Quaternion;
struct CVector;
class CPlane;
struct CMatrix;
struct CVector4;

struct CMatrix { /* Size=0x40 */
  union {
    struct {
      /* 0x0000 */ public: float _11;
      /* 0x0004 */ public: float _12;
      /* 0x0008 */ public: float _13;
      /* 0x000c */ public: float _14;
      /* 0x0010 */ public: float _21;
      /* 0x0014 */ public: float _22;
      /* 0x0018 */ public: float _23;
      /* 0x001c */ public: float _24;
      /* 0x0020 */ public: float _31;
      /* 0x0024 */ public: float _32;
      /* 0x0028 */ public: float _33;
      /* 0x002c */ public: float _34;
      /* 0x0030 */ public: float _41;
      /* 0x0034 */ public: float _42;
      /* 0x0038 */ public: float _43;
      /* 0x003c */ public: float _44;
    };
    /* 0x0000 */ public: float m[4][4];
  };
  
  public: CMatrix(const CMatrix&);
  public: CMatrix();
  public: void zero();
  public: void identity();
  public: void translation(const CVector&);
  public: void translation(float, float, float);
  public: void scaling(float);
  public: void scaling(float, float, float);
  public: void rotX(float);
  public: void rotY(float);
  public: void rotZ(float);
  public: float calcDeterminant() const;
  public: float calcDeterminantSimple() const;
  public: CMatrix getInverse() const;
  public: CMatrix getInverseRot() const;
  public: CMatrix getInverseRotTranslate() const;
  public: CMatrix getInverseSimple() const;
  public: void rotAxis(const CVector&, float);
  public: void rotTranslate(const Quaternion&, const CVector&);
  public: void rotYPR(float, float, float);
  public: void getYPR(float&, float&, float&) const;
  public: void GetBasis(CVector&, CVector&, CVector&) const;
  public: void FromBasis(const CVector&, const CVector&, const CVector&);
  public: void GetNormalizedBasis(CVector&, CVector&, CVector&) const;
  public: void GetInvBasis(CVector&, CVector&, CVector&) const;
  public: void FromInvBasis(const CVector&, const CVector&, const CVector&);
  public: void GetInvNormalizedBasis(CVector&, CVector&, CVector&) const;
  public: CVector getOrg() const;
  public: void setOrg(const CVector&);
  public: CVector getOrgInv() const;
  public: void composeSRT(const CVector&, const CMatrix&, const CVector&);
  public: float GetScaleX() const;
  public: float GetScaleY() const;
  public: float GetScaleZ() const;
  public: void lookAtLH(const CVector&, const CVector&, const CVector&);
  public: CMatrix getTransposed() const;
  public: void transposeInplace();
  public: void orthoLH(float, float, float, float);
  public: void perspectiveFovLH(float, float, float, float);
  public: void perspectiveLH(float, float, float, float);
  public: void reflect(const CPlane&);
  public: void shadow(const CVector4&, const CPlane&);
  public: void shearXbyYZ(float, float);
  public: void shearYbyXZ(float, float);
  public: void shearZbyXY(float, float);
  public: void shear(float, float, float, float, float, float);
  public: CVector vecRotBack(const CVector&) const;
  public: CVector vecRot(const CVector&) const;
  public: CVector vecMulBack(const CVector&) const;
  public: CVector4 vecMul(const CVector4&) const;
  public: CVector vecMul(const CVector&) const;
  public: void vecRotBackInplace(const CVector&, CVector&) const;
  public: void vecRotInplace(const CVector&, CVector&) const;
  public: void vecMulBackInplace(const CVector&, CVector&) const;
  public: void vecMulInplace(const CVector4&, CVector4&) const;
  public: void vecMulInplace(const CVector&, CVector&) const;
  public: void DiagMatrixMul(const CVector&);
  public: float& operator()(int32_t, int32_t);
  public: float operator()(int32_t, int32_t) const;
  public: void operator*=(const CMatrix&);
  public: void DecomposeScale(float&, float&, float&);
  public: void createPlaneTransform();
  public: void transformPlane(CVector4&);
};
