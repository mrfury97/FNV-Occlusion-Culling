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

	// A box's 8 corners, in the order BoxTriangles uses (bit 0 = x max, bit 1 = y max, bit 2 = z max as in kCorner).
	static const int kCorner[8][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 },
	                                   { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 } };

	inline void AxisCorners(const float* mn, const float* mx, float (*out)[3]) {
		for (int k = 0; k < 8; ++k)
			for (int i = 0; i < 3; ++i) out[k][i] = kCorner[k][i] ? mx[i] : mn[i];
	}

	// Oriented box: local box [lmn, lmx] placed by rotation rot (row-major 3x3, world = rot * local, as NiMatrix3) at org.
	inline void OrientedCorners(const float* rot, const float* org, const float* lmn, const float* lmx, float (*out)[3]) {
		for (int k = 0; k < 8; ++k) {
			const float l[3] = { kCorner[k][0] ? lmx[0] : lmn[0], kCorner[k][1] ? lmx[1] : lmn[1], kCorner[k][2] ? lmx[2] : lmn[2] };
			for (int i = 0; i < 3; ++i) out[k][i] = org[i] + rot[i * 3 + 0] * l[0] + rot[i * 3 + 1] * l[1] + rot[i * 3 + 2] * l[2];
		}
	}

	// World point into the oriented box's frame: rot^T * (p - org).
	inline void ToLocal(const float* rot, const float* org, const float* p, float* out) {
		const float d[3] = { p[0] - org[0], p[1] - org[1], p[2] - org[2] };
		for (int i = 0; i < 3; ++i) out[i] = rot[0 * 3 + i] * d[0] + rot[1 * 3 + i] * d[1] + rot[2 * 3 + i] * d[2];
	}

	// The axis-aligned box holding box [a, b] after x -> m * x + o (m row-major 3x3): centre moved, half sizes |m| * half.
	inline void TransformedBox(const float* m, const float* o, const float* a, const float* b, float* mn, float* mx) {
		const float c[3] = { (a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f, (a[2] + b[2]) * 0.5f };
		const float e[3] = { (b[0] - a[0]) * 0.5f, (b[1] - a[1]) * 0.5f, (b[2] - a[2]) * 0.5f };
		for (int i = 0; i < 3; ++i) {
			const float* r = m + i * 3;
			const float ci = o[i] + r[0] * c[0] + r[1] * c[1] + r[2] * c[2];
			const float ei = std::fabs(r[0]) * e[0] + std::fabs(r[1]) * e[1] + std::fabs(r[2]) * e[2];
			mn[i] = ci - ei;
			mx[i] = ci + ei;
		}
	}

	inline float BoxVolume(const float* mn, const float* mx) {
		return (mx[0] - mn[0]) * (mx[1] - mn[1]) * (mx[2] - mn[2]);
	}

	// The 12 triangles of a box as indices into its 8 corners (kCorner order): an index buffer for 8-vertex boxes.
	static const uint16_t kBoxIndices[36] = { 0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
	                                          3, 6, 2, 3, 7, 6, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5 };

	// A box's 8 corners (world), projected like ProjectToScreen.
	inline bool ProjectCornerPoints(const float (*corners)[3], const float* origin, const float* view, const float* proj,
		float vpX, float vpY, float vpW, float vpH, float minZ, float maxZ, uint32_t color, ScreenVertex* out8) {
		Vertex c[8];
		for (int k = 0; k < 8; ++k) c[k] = { corners[k][0] - origin[0], corners[k][1] - origin[1], corners[k][2] - origin[2], color };
		return ProjectToScreen(c, 8, view, proj, vpX, vpY, vpW, vpH, minZ, maxZ, out8);
	}

	// How far (pixels) a moving camera can slide an occluder's edge across the screen in one frame: an edge at view
	// depth `depth` moves halfWidth * proj11 * move / depth pixels for a sideways camera move of `move` units (things
	// farther behind it move less, so this bounds what the edge uncovers). Capped at `cap` pixels.
	inline float RevealPixels(float halfWidth, float proj11, float move, float depth, float cap) {
		if (!(move > 0.0f) || !(depth > 0.0f)) return 0.0f;
		const float px = halfWidth * std::fabs(proj11) * move / depth;
		return px < cap ? px : cap;
	}

	// Grows a projected box on screen: each corner moves `px` pixels away from the corners' centre along x and y, so
	// the box covers at least `px` more on every side of its screen rectangle. Depths are kept.
	inline void GrowOnScreen(ScreenVertex* s8, float px) {
		float cx = 0.0f, cy = 0.0f;
		for (int k = 0; k < 8; ++k) { cx += s8[k].x; cy += s8[k].y; }
		cx *= 0.125f; cy *= 0.125f;
		for (int k = 0; k < 8; ++k) {
			s8[k].x += s8[k].x >= cx ? px : -px;
			s8[k].y += s8[k].y >= cy ? px : -px;
		}
	}

	// The 36 triangle-list vertices of 8 projected corners.
	inline void ExpandBox(const ScreenVertex* s8, ScreenVertex* out36) {
		for (int k = 0; k < 36; ++k) out36[k] = s8[kBoxIndices[k]];
	}

	// The 36 triangle-list vertices of a box given by its 8 corners (world), projected like ProjectToScreen.
	inline bool ProjectCorners(const float (*corners)[3], const float* origin, const float* view, const float* proj,
		float vpX, float vpY, float vpW, float vpH, float minZ, float maxZ, uint32_t color, ScreenVertex* out) {
		ScreenVertex s[8];
		if (!ProjectCornerPoints(corners, origin, view, proj, vpX, vpY, vpW, vpH, minZ, maxZ, color, s)) return false;
		ExpandBox(s, out);
		return true;
	}

	// The same as BoxTriangles + ProjectToScreen, projecting only the 8 corners (the 36 vertices are copies of them).
	inline bool ProjectBox(const float* mn, const float* mx, const float* origin, const float* view, const float* proj,
		float vpX, float vpY, float vpW, float vpH, float minZ, float maxZ, uint32_t color, ScreenVertex* out) {
		float corners[8][3];
		AxisCorners(mn, mx, corners);
		return ProjectCorners(corners, origin, view, proj, vpX, vpY, vpW, vpH, minZ, maxZ, color, out);
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

	inline float DistanceToOrientedBox(const float* p, const float* rot, const float* org, const float* lmn, const float* lmx) {
		float l[3];
		ToLocal(rot, org, p, l);
		return DistanceToBox(l, lmn, lmx);
	}
}
