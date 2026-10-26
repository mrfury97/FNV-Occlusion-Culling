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

	// Pre-transformed vertex (D3DFVF_XYZRHW | D3DFVF_DIFFUSE): viewport pixels, depth in [MinZ, MaxZ], 1 / w.
	struct ScreenVertex { float x, y, z, rhw; uint32_t color; };

	// Camera-relative points through view * projection (D3D row vectors, 16 floats each, row-major) into viewport pixels.
	// False when any point is not in front of the camera (w <= 1 unit): such a box cannot be drawn pre-transformed.
	inline bool ProjectToScreen(const Vertex* in, int count, const float* view, const float* proj, float vpX, float vpY,
		float vpW, float vpH, float minZ, float maxZ, ScreenVertex* out) {
		for (int k = 0; k < count; ++k) {
			const float p[4] = { in[k].x, in[k].y, in[k].z, 1.0f };
			float v[4], c[4];
			for (int j = 0; j < 4; ++j) v[j] = p[0] * view[0 * 4 + j] + p[1] * view[1 * 4 + j] + p[2] * view[2 * 4 + j] + p[3] * view[3 * 4 + j];
			for (int j = 0; j < 4; ++j) c[j] = v[0] * proj[0 * 4 + j] + v[1] * proj[1 * 4 + j] + v[2] * proj[2 * 4 + j] + v[3] * proj[3 * 4 + j];
			if (!(c[3] > 1.0f)) return false;
			const float iw = 1.0f / c[3];
			out[k] = { vpX + (c[0] * iw * 0.5f + 0.5f) * vpW - 0.5f, vpY + (0.5f - c[1] * iw * 0.5f) * vpH - 0.5f,
			           minZ + c[2] * iw * (maxZ - minZ), iw, in[k].color };
		}
		return true;
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
