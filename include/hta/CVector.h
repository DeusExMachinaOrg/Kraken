#pragma once

template <typename T>
struct PointBase
{
	T x;
	T y;
};

struct CVector {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;


    // Modern API
	CVector() = default;
	CVector(float x, float y, float z): x(x), y(y), z(z) {};
	CVector(float* d): x(d[0]), y(d[1]), z(d[2]) {};
	float length() const { return sqrtf(x*x + y*y + z*z); };

	CVector& operator += (const CVector& other) {
		x += other.x;
		y += other.y;
		z += other.z;
		return *this;
	};
	CVector& operator *= (float other) {
		x *= other;
		y *= other;
		z *= other;
		return *this;
	};
	CVector& operator *= (const CVector& other) {
		x *= other.x;
		y *= other.y;
		z *= other.z;
		return *this;
	};
};