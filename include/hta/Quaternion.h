#pragma once
#include "Game.h"

struct Quaternion
{
    float x;
    float y;
    float z;
    float w;

    inline static Quaternion& IdentityQuaternion_28 = *(Quaternion*)0x00A04D80;

    Quaternion()
    {
        this->Zero();
    }

    void Lerp(const Quaternion* q1, const Quaternion* q2, float k2)
    {
        FUNC(0x0070AFC0, void, __thiscall, _Lerp, Quaternion*, const Quaternion*, const Quaternion*, float);
        _Lerp(this, q1, q2, k2);
    }

    void Slerp(const Quaternion* q1, const Quaternion* q2, float k2)
    {
        FUNC(0x005FED10, void, __fastcall, _Lerp, Quaternion*, const Quaternion*, const Quaternion*, float);
        _Lerp(this, q1, q2, k2);
    }

    void Zero()
    {
        this->x = 0.0;
        this->y = 0.0;
        this->z = 0.0;
        this->w = 0.0;
    }

    void RotX(float radians)
    {
        long double v2; // st7

        v2 = radians * 0.5;
        this->y = 0.0;
        this->z = 0.0;
        this->x = sin(v2);
        this->w = cos(v2);
    }

    void RotY(float radians)
    {
        long double v2; // st7

        v2 = radians * 0.5;
        this->x = 0.0;
        this->z = 0.0;
        this->y = sin(v2);
        this->w = cos(v2);
    }

    void RotZ(float radians)
    {
        long double v2; // st7

        v2 = radians * 0.5;
        this->x = 0.0;
        this->y = 0.0;
        this->z = sin(v2);
        this->w = cos(v2);
    }

    // Modern API
    Quaternion(float x, float y, float z, float w): x(x), y(y), z(z), w(w) {};
    Quaternion(const Quaternion& other): x(other.x), y(other.y), z(other.z), w(other.w) {};
    Quaternion operator ~() {
        float length = sqrtf(x*x + y*y + z*z + w*w);
        if (length <= 0.0f) return Quaternion();
        return Quaternion(-x / length, -y / length, -z / length, w / length);
    };
    CVector operator *(CVector other) {
            float xx = x * x, yy = y * y, zz = z * z;
            float xy = x * y, xz = x * z, xw = x * w;
            float yz = y * z, yw = y * w, zw = z * w;
            float m11 = 1.0f - 2.0f * (yy + zz);
            float m12 = 2.0f * (xy + zw);
            float m13 = 2.0f * (xz - yw);
            float m21 = 2.0f * (xy - zw);
            float m22 = 1.0f - 2.0f * (xx + zz);
            float m23 = 2.0f * (yz + xw);
            float m31 = 2.0f * (xz + yw);
            float m32 = 2.0f * (yz - xw);
            float m33 = 1.0f - 2.0f * (xx + yy);
            return CVector(
                other.x * m11 + other.y * m21 + other.z * m31,
                other.x * m12 + other.y * m22 + other.z * m32,
                other.x * m13 + other.y * m23 + other.z * m33
            );
    };
};