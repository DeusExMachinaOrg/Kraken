#pragma once
#include "CVector.h"

class CVector4;
class Quaternion;

class CPlane
{
public:
    static void buildplane(CPlane*, CVector*);

public:
    CPlane();
    CPlane(CPlane const&);
    void fromPointNormal(CVector const&, CVector const&);
    float intersectRay(CVector const&, CVector const&);
    CPlane Reverse();
    float dist(CVector const&);
    CVector origin();

private:
    CVector m_normal;
    float m_dist;
    unsigned int m_type;
    unsigned int m_signbits;
};



class CMatrix
{
public:
    CMatrix(CMatrix const&);
    CMatrix();

    CVector vecRot(CVector const&) const;
    CMatrix getInverseRotTranslate() const;
    void zero();
    CMatrix getTransposed() const;
    float GetScaleZ() const;
    CMatrix getInverseRot() const;
    void FromInvBasis(CVector const&, CVector const&, CVector const&);
    void DecomposeScale(float&, float&, float&);
    float GetScaleX() const;
    void shadow(CVector4 const&, CPlane const&);
    void GetInvBasis(CVector&, CVector&, CVector&) const;
    void composeSRT(CVector const&, CMatrix const&, CVector const&);
    void reflect(CPlane const&);
    void translation(CVector const&);
    void translation(float, float, float);
    void getYPR(float&, float&, float&) const;
    void rotTranslate(Quaternion const&, CVector const&);
    void GetNormalizedBasis(CVector&, CVector&, CVector&) const;
    CVector vecMul(CVector const&) const;
    CVector4 vecMul(CVector4 const&) const;
    void GetBasis(CVector&, CVector&, CVector&) const;
    CVector getOrg() const;
    CVector vecRotBack(CVector const&) const;
    void FromBasis(CVector const&, CVector const&, CVector const&);
    CMatrix getInverse() const;
    void operator*=(CMatrix const&);
    CVector getOrgInv() const;
    void perspectiveFovLH(float, float, float, float);
    void rotZ(float);
    void rotY(float);
    void rotX(float);
    void orthoLH(float, float, float, float);
    void rotYPR(float, float, float);
    float operator()(int, int) const;
    float& operator()(int, int);
    void setOrg(CVector const&);
    void scaling(float);
    void scaling(float, float, float);
    void identity();
    void lookAtLH(CVector const&, CVector const&, CVector const&);
    void shear(float, float, float, float, float, float);

public:
    union
    {
        struct
        {
            float _11;
            float _12;
            float _13;
            float _14;
            float _21;
            float _22;
            float _23;
            float _24;
            float _31;
            float _32;
            float _33;
            float _34;
            float _41;
            float _42;
            float _43;
            float _44;
        };
        float m[4][4];
    };
};

ASSERT_SIZE(CPlane, 0x18);
ASSERT_SIZE(CMatrix, 0x40);
