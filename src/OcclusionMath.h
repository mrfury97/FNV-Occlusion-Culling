#pragma once
// Engine-free helpers, shared with tests\math_test.cpp.

#include <cmath>
#include <cstdint>

namespace OcclusionMath {

	// D3D row-vector projection (clip = [x y z 1] * P): does NDC depth grow with view distance (standard: compare
	// LESSEQUAL) or shrink (reversed: GREATEREQUAL)? p is the 4x4 matrix as 16 floats, row-major (_11.._44).
	inline bool DepthGrowsWithDistance(const float* p) {
		auto ndc = [&](float d) {
			const float z = d * p[2 * 4 + 2] + p[3 * 4 + 2];
			const float w = d * p[2 * 4 + 3] + p[3 * 4 + 3];
			return w != 0.0f ? z / w : 0.0f;
		};
		return ndc(10000.0f) >= ndc(100.0f);
	}

	// Angle in degrees between two rotations (row-major 3x3): acos((trace(A^T B) - 1) / 2).
	inline float RotationAngleDegrees(const float* a, const float* b) {
		float trace = 0.0f;
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c) trace += a[r * 3 + c] * b[r * 3 + c];
		float x = (trace - 1.0f) * 0.5f;
		if (x > 1.0f) x = 1.0f;
		if (x < -1.0f) x = -1.0f;
		return std::acos(x) * 57.2957795f;
	}

	// The 36 corners (12 triangles) of an axis-aligned box, minus `origin` (the renderer draws camera-relative).
	struct Vertex { float x, y, z; uint32_t color; };
	inline void BoxTriangles(const float* mn, const float* mx, const float* origin, uint32_t color, Vertex* out) {
		static const int corner[8][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 },
		                                  { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 } };
		static const int tri[12][3] = { { 0, 2, 1 }, { 0, 3, 2 }, { 4, 5, 6 }, { 4, 6, 7 }, { 0, 1, 5 }, { 0, 5, 4 },
		                                { 3, 6, 2 }, { 3, 7, 6 }, { 0, 4, 7 }, { 0, 7, 3 }, { 1, 2, 6 }, { 1, 6, 5 } };
		for (int t = 0; t < 12; ++t)
			for (int i = 0; i < 3; ++i) {
				const int* c = corner[tri[t][i]];
				out[t * 3 + i] = { (c[0] ? mx[0] : mn[0]) - origin[0], (c[1] ? mx[1] : mn[1]) - origin[1],
				                   (c[2] ? mx[2] : mn[2]) - origin[2], color };
			}
	}

	// Distance from p to the nearest point of the box (0 inside).
	inline float DistanceToBox(const float* p, const float* mn, const float* mx) {
		float d2 = 0.0f;
		for (int i = 0; i < 3; ++i) {
			const float v = p[i] < mn[i] ? mn[i] - p[i] : p[i] > mx[i] ? p[i] - mx[i] : 0.0f;
			d2 += v * v;
		}
		return std::sqrt(d2);
	}
}
