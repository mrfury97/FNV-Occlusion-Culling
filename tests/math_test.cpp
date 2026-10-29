// Offline checks for OcclusionMath.h, with matrices built exactly as NiDX9Renderer::Do_SetCameraData builds them.
#include <cstdio>
#include <cmath>
#include "../src/OcclusionMath.h"

static int gFails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++gFails; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Perspective projection as Do_SetCameraData (not left-handed branch): rows _11.._44.
static void Proj(float l, float r, float t, float b, float n, float f, float* p) {
	for (int i = 0; i < 16; ++i) p[i] = 0;
	const float inv = 1.0f / (f - n);
	p[0] = 2.0f / (r - l);
	p[2 * 4 + 0] = -(l + r) / (r - l);
	p[5] = 2.0f / (t - b);
	p[2 * 4 + 1] = -(t + b) / (t - b);
	p[2 * 4 + 2] = inv * f;
	p[3 * 4 + 2] = -(inv * n * f);
	p[2 * 4 + 3] = 1.0f;
}

// View as Do_SetCameraData with kPosAdjust = camera position (translation 0): columns right, up, dir.
static void View(const float* right, const float* up, const float* dir, float* v) {
	for (int i = 0; i < 16; ++i) v[i] = 0;
	for (int r = 0; r < 3; ++r) { v[r * 4 + 0] = right[r]; v[r * 4 + 1] = up[r]; v[r * 4 + 2] = dir[r]; }
	v[15] = 1;
}

static void Mul(const float* x4, const float* m, float* out) {   // row vector * matrix
	for (int c = 0; c < 4; ++c) out[c] = x4[0] * m[c] + x4[1] * m[4 + c] + x4[2] * m[8 + c] + x4[3] * m[12 + c];
}

int main() {
	// FNV-like frustum: 75 degree horizontal, 16:9, near 10, far 300000.
	const float tx = std::tan(37.5f * 0.0174533f), ty = tx * 9.0f / 16.0f;
	float p[16];
	Proj(-tx, tx, ty, -ty, 10.0f, 300000.0f, p);
	CHECK(OcclusionMath::DepthGrowsWithDistance(p), "standard projection detected as reversed");
	float rev[16];
	Proj(-tx, tx, ty, -ty, 300000.0f, 10.0f, rev);   // swapped near/far = reversed depth
	CHECK(!OcclusionMath::DepthGrowsWithDistance(rev), "reversed projection detected as standard");

	// Camera at a far world position looking along +Y (heading 0), right = +X, up = +Z.
	const float cam[3] = { 98000.0f, -18500.0f, 3000.0f };
	const float right[3] = { 1, 0, 0 }, up[3] = { 0, 0, 1 }, dir[3] = { 0, 1, 0 };
	float v[16];
	View(right, up, dir, v);
	// A box 2000 units ahead: its camera-relative corners must project inside the screen, with depth in (0, 1).
	const float mn[3] = { cam[0] - 300, cam[1] + 1800, cam[2] - 200 }, mx[3] = { cam[0] + 300, cam[1] + 2400, cam[2] + 400 };
	OcclusionMath::Vertex tri[36];
	OcclusionMath::BoxTriangles(mn, mx, cam, 0xFFFFFFFF, tri);
	int inside = 0;
	for (const auto& t : tri) {
		const float x4[4] = { t.x, t.y, t.z, 1 };
		float vc[4], clip[4];
		Mul(x4, v, vc);
		Mul(vc, p, clip);
		const float nx = clip[0] / clip[3], ny = clip[1] / clip[3], nz = clip[2] / clip[3];
		if (std::fabs(nx) <= 1 && std::fabs(ny) <= 1 && nz > 0 && nz < 1) ++inside;
	}
	CHECK(inside == 36, "box ahead: only %d of 36 vertices on screen", inside);
	// Every corner appears in the triangles exactly as a corner of [mn, mx] minus the camera.
	for (const auto& t : tri) {
		CHECK((t.x == mn[0] - cam[0] || t.x == mx[0] - cam[0]) && (t.y == mn[1] - cam[1] || t.y == mx[1] - cam[1]) &&
			(t.z == mn[2] - cam[2] || t.z == mx[2] - cam[2]), "bad corner");
	}
	// A nearer depth value for a nearer point (the depth test compares the right way round).
	{
		const float a4[4] = { 0, 500, 0, 1 }, b4[4] = { 0, 5000, 0, 1 };
		float va[4], vb[4], ca[4], cb[4];
		Mul(a4, v, va); Mul(va, p, ca); Mul(b4, v, vb); Mul(vb, p, cb);
		CHECK(ca[2] / ca[3] < cb[2] / cb[3], "depth does not grow with distance");
	}

	// CPU projection matches the matrix maths: the box ahead lands on screen with depth between 0 and 1, the nearer face
	// nearer (standard depth) or farther (reversed depth); a box reaching behind the camera is refused.
	{
		OcclusionMath::ScreenVertex sv[36];
		CHECK(OcclusionMath::ProjectToScreen(tri, 36, v, p, 0, 0, 1920, 1080, 0, 1, sv), "box ahead refused");
		bool onScreen = true;
		for (const auto& x : sv) onScreen = onScreen && x.x >= -0.5f && x.x <= 1919.5f && x.y >= -0.5f && x.y <= 1079.5f && x.z > 0 && x.z < 1;
		CHECK(onScreen, "projected box ahead is off screen");
		const OcclusionMath::Vertex nearPt = { 0, 500, 0, 0 }, farPt = { 0, 5000, 0, 0 };
		OcclusionMath::ScreenVertex sn, sf;
		CHECK(OcclusionMath::ProjectToScreen(&nearPt, 1, v, p, 0, 0, 1920, 1080, 0, 1, &sn) &&
			OcclusionMath::ProjectToScreen(&farPt, 1, v, p, 0, 0, 1920, 1080, 0, 1, &sf) && sn.z < sf.z, "standard depth order");
		CHECK(OcclusionMath::ProjectToScreen(&nearPt, 1, v, rev, 0, 0, 1920, 1080, 0, 1, &sn) &&
			OcclusionMath::ProjectToScreen(&farPt, 1, v, rev, 0, 0, 1920, 1080, 0, 1, &sf) && sn.z > sf.z, "reversed depth order");
		CHECK(std::fabs(sn.x - 959.5f) < 0.01f && std::fabs(sn.y - 539.5f) < 0.01f, "point straight ahead not at the centre (%.2f, %.2f)", sn.x, sn.y);
		const float bmn2[3] = { cam[0] - 300, cam[1] - 100, cam[2] - 200 }, bmx2[3] = { cam[0] + 300, cam[1] + 600, cam[2] + 200 };
		OcclusionMath::Vertex t2[36];
		OcclusionMath::BoxTriangles(bmn2, bmx2, cam, 0xFFFFFFFF, t2);
		CHECK(!OcclusionMath::ProjectToScreen(t2, 36, v, p, 0, 0, 1920, 1080, 0, 1, sv), "box reaching behind the camera accepted");
		// ProjectBox (8 corners) gives exactly the vertices of BoxTriangles + ProjectToScreen.
		OcclusionMath::ScreenVertex sb[36];
		OcclusionMath::ProjectToScreen(tri, 36, v, p, 0, 0, 1920, 1080, 0, 1, sv);   // sv again: the refused box above overwrote part of it
		CHECK(OcclusionMath::ProjectBox(mn, mx, cam, v, p, 0, 0, 1920, 1080, 0, 1, 0xFFFFFFFF, sb), "ProjectBox refused the box ahead");
		bool same = true;
		for (int k = 0; k < 36; ++k)
			same = same && sb[k].x == sv[k].x && sb[k].y == sv[k].y && sb[k].z == sv[k].z && sb[k].rhw == sv[k].rhw;
		CHECK(same, "ProjectBox differs from BoxTriangles + ProjectToScreen");
		CHECK(!OcclusionMath::ProjectBox(bmn2, bmx2, cam, v, p, 0, 0, 1920, 1080, 0, 1, 0xFFFFFFFF, sb), "ProjectBox accepted a box behind the camera");
		// 8 projected corners + kBoxIndices (the batched draw) give exactly the 36 vertices of the per-box draw.
		float ac[8][3];
		OcclusionMath::AxisCorners(mn, mx, ac);
		OcclusionMath::ScreenVertex s8[8], s36[36];
		CHECK(OcclusionMath::ProjectCornerPoints(ac, cam, v, p, 0, 0, 1920, 1080, 0, 1, 0xFFFFFFFF, s8), "corner points refused");
		OcclusionMath::ExpandBox(s8, s36);
		bool indexed = true;
		for (int k = 0; k < 36; ++k)
			indexed = indexed && s36[k].x == sv[k].x && s36[k].y == sv[k].y && s36[k].z == sv[k].z && s36[k].rhw == sv[k].rhw;
		CHECK(indexed, "indexed corners differ from the 36-vertex box");
		// Growing on screen: the corners' rectangle grows by exactly px on every side, depths stay.
		{
			OcclusionMath::ScreenVertex g8[8];
			for (int k = 0; k < 8; ++k) g8[k] = s8[k];
			float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, gx0 = 1e9f, gx1 = -1e9f, gy0 = 1e9f, gy1 = -1e9f;
			OcclusionMath::GrowOnScreen(g8, 25.0f);
			bool depths = true;
			for (int k = 0; k < 8; ++k) {
				x0 = s8[k].x < x0 ? s8[k].x : x0; x1 = s8[k].x > x1 ? s8[k].x : x1;
				y0 = s8[k].y < y0 ? s8[k].y : y0; y1 = s8[k].y > y1 ? s8[k].y : y1;
				gx0 = g8[k].x < gx0 ? g8[k].x : gx0; gx1 = g8[k].x > gx1 ? g8[k].x : gx1;
				gy0 = g8[k].y < gy0 ? g8[k].y : gy0; gy1 = g8[k].y > gy1 ? g8[k].y : gy1;
				depths = depths && g8[k].z == s8[k].z && g8[k].rhw == s8[k].rhw;
			}
			CHECK(std::fabs((x0 - gx0) - 25.0f) < 0.01f && std::fabs((gx1 - x1) - 25.0f) < 0.01f &&
				std::fabs((y0 - gy0) - 25.0f) < 0.01f && std::fabs((gy1 - y1) - 25.0f) < 0.01f,
				"grown rectangle: %.2f %.2f %.2f %.2f", x0 - gx0, gx1 - x1, y0 - gy0, gy1 - y1);
			CHECK(depths, "growing changed depths");
		}
		// Reveal distance: 1920 wide, 75 degree view (proj11 1.303), 6 units of sideways move, edge 100 units away:
		// 960 * 1.303 * 6 / 100 = 75 px; nothing when still; capped.
		CHECK(std::fabs(OcclusionMath::RevealPixels(960.0f, 1.303f, 6.0f, 100.0f, 480.0f) - 75.05f) < 0.1f, "reveal px %.2f",
			OcclusionMath::RevealPixels(960.0f, 1.303f, 6.0f, 100.0f, 480.0f));
		CHECK(OcclusionMath::RevealPixels(960.0f, 1.303f, 0.0f, 100.0f, 480.0f) == 0.0f, "reveal while still");
		CHECK(OcclusionMath::RevealPixels(960.0f, 1.303f, 500.0f, 100.0f, 480.0f) == 480.0f, "reveal not capped");
	}

	// Rotation angle: identity vs itself 0, vs a 10 degree yaw 10.
	const float I[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
	const float c = std::cos(10 * 0.0174533f), s = std::sin(10 * 0.0174533f);
	const float Z[9] = { c, s, 0, -s, c, 0, 0, 0, 1 };
	CHECK(std::fabs(OcclusionMath::RotationAngleDegrees(I, I)) < 0.05f, "identity angle");
	CHECK(std::fabs(OcclusionMath::RotationAngleDegrees(I, Z) - 10.0f) < 0.05f, "10 degree angle %.3f",
		OcclusionMath::RotationAngleDegrees(I, Z));

	// Oriented boxes: a 30 degree yaw (world = R * local, R row-major as NiMatrix3).
	{
		const float a = 30 * 0.0174533f, ca = std::cos(a), sa = std::sin(a);
		const float R[9] = { ca, -sa, 0, sa, ca, 0, 0, 0, 1 };
		const float org[3] = { 1000, 2000, 300 };
		const float lmn[3] = { -400, -20, 0 }, lmx[3] = { 400, 20, 300 };   // a long thin wall
		float corners[8][3];
		OcclusionMath::OrientedCorners(R, org, lmn, lmx, corners);
		// Each corner back in the local frame is a corner of [lmn, lmx].
		bool back = true;
		for (int k = 0; k < 8; ++k) {
			float l[3];
			OcclusionMath::ToLocal(R, org, corners[k], l);
			for (int i = 0; i < 3; ++i)
				back = back && std::fabs(l[i] - (OcclusionMath::kCorner[k][i] ? lmx[i] : lmn[i])) < 0.01f;
		}
		CHECK(back, "oriented corners do not map back to the local box");
		// Corner 1 (x max) is the wall's end: org + R * (400, -20, 0).
		CHECK(std::fabs(corners[1][0] - (org[0] + ca * 400 + sa * 20)) < 0.01f && std::fabs(corners[1][1] - (org[1] + sa * 400 - ca * 20)) < 0.01f,
			"corner 1 at (%.2f, %.2f)", corners[1][0], corners[1][1]);
		// Distance: from the wall's middle, 100 units out along its local y, is 80.
		const float local[3] = { 0, 100, 150 };
		const float p[3] = { org[0] + R[0] * local[0] + R[1] * local[1], org[1] + R[3] * local[0] + R[4] * local[1], org[2] + local[2] };
		CHECK(std::fabs(OcclusionMath::DistanceToOrientedBox(p, R, org, lmn, lmx) - 80.0f) < 0.01f, "oriented distance %.3f",
			OcclusionMath::DistanceToOrientedBox(p, R, org, lmn, lmx));
		// The oriented box is far smaller than the world-axis box around it.
		float wmn[3], wmx[3];
		OcclusionMath::TransformedBox(R, org, lmn, lmx, wmn, wmx);
		CHECK(OcclusionMath::BoxVolume(lmn, lmx) < 0.25f * OcclusionMath::BoxVolume(wmn, wmx), "oriented box not smaller");
		// TransformedBox holds every transformed corner, and touches the extremes.
		bool holds = true;
		float emn[3] = { 1e30f, 1e30f, 1e30f }, emx[3] = { -1e30f, -1e30f, -1e30f };
		for (int k = 0; k < 8; ++k)
			for (int i = 0; i < 3; ++i) {
				holds = holds && corners[k][i] >= wmn[i] - 0.01f && corners[k][i] <= wmx[i] + 0.01f;
				emn[i] = corners[k][i] < emn[i] ? corners[k][i] : emn[i];
				emx[i] = corners[k][i] > emx[i] ? corners[k][i] : emx[i];
			}
		for (int i = 0; i < 3; ++i) holds = holds && std::fabs(emn[i] - wmn[i]) < 0.01f && std::fabs(emx[i] - wmx[i]) < 0.01f;
		CHECK(holds, "TransformedBox is not the transformed corners' extent");
		// Scaled shape inside the object: x -> R^T * (R_g * 2 * x + t_g - org), as WalkShapes composes it.
		const float Rg[9] = { ca, -sa, 0, sa, ca, 0, 0, 0, 1 };   // same rotation as the root: no growth expected
		const float tg[3] = { 1100, 2050, 300 }, amn[3] = { -10, -5, 0 }, amx[3] = { 10, 5, 50 };
		float mw[9], ml[9], ol[3], smn[3], smx[3];
		for (int k = 0; k < 9; ++k) mw[k] = Rg[k] * 2.0f;
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j) ml[i * 3 + j] = R[0 * 3 + i] * mw[0 * 3 + j] + R[1 * 3 + i] * mw[1 * 3 + j] + R[2 * 3 + i] * mw[2 * 3 + j];
		OcclusionMath::ToLocal(R, org, tg, ol);
		OcclusionMath::TransformedBox(ml, ol, amn, amx, smn, smx);
		CHECK(std::fabs((smx[0] - smn[0]) - 40) < 0.01f && std::fabs((smx[1] - smn[1]) - 20) < 0.01f && std::fabs((smx[2] - smn[2]) - 100) < 0.01f,
			"aligned scaled shape grew: %.2f x %.2f x %.2f", smx[0] - smn[0], smx[1] - smn[1], smx[2] - smn[2]);
		// and its local box, placed back in the world, holds the shape's world corners
		float sc[8][3], wc[8][3];
		OcclusionMath::OrientedCorners(R, org, smn, smx, sc);
		OcclusionMath::AxisCorners(amn, amx, wc);
		bool inside = true;
		for (int k = 0; k < 8; ++k) {
			float w[3], l[3];
			for (int i = 0; i < 3; ++i) w[i] = tg[i] + mw[i * 3 + 0] * wc[k][0] + mw[i * 3 + 1] * wc[k][1] + mw[i * 3 + 2] * wc[k][2];
			OcclusionMath::ToLocal(R, org, w, l);
			for (int i = 0; i < 3; ++i) inside = inside && l[i] >= smn[i] - 0.01f && l[i] <= smx[i] + 0.01f;
		}
		CHECK(inside, "shape corner outside its local box");
		// ProjectCorners of an identity-rotated box equals ProjectBox.
		const float I3[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, zero[3] = { 0, 0, 0 };
		float ic[8][3];
		OcclusionMath::OrientedCorners(I3, zero, mn, mx, ic);
		OcclusionMath::ScreenVertex s1[36], s2[36];
		CHECK(OcclusionMath::ProjectCorners(ic, cam, v, p, 0, 0, 1920, 1080, 0, 1, 0xFFFFFFFF, s1) &&
			OcclusionMath::ProjectBox(mn, mx, cam, v, p, 0, 0, 1920, 1080, 0, 1, 0xFFFFFFFF, s2), "identity corners refused");
		bool same = true;
		for (int k = 0; k < 36; ++k) same = same && s1[k].x == s2[k].x && s1[k].y == s2[k].y && s1[k].z == s2[k].z;
		CHECK(same, "ProjectCorners(identity) differs from ProjectBox");
	}

	// Distance to box.
	const float bmn[3] = { 0, 0, 0 }, bmx[3] = { 10, 10, 10 };
	const float in[3] = { 5, 5, 5 }, out[3] = { 13, 14, 10 };
	CHECK(OcclusionMath::DistanceToBox(in, bmn, bmx) == 0.0f, "inside");
	CHECK(std::fabs(OcclusionMath::DistanceToBox(out, bmn, bmx) - 5.0f) < 1e-4f, "outside");

	// Clipping at the camera (ProjectCornersClipped).
	{
		float ahead[8][3], straddle[8][3], behind[8][3];
		OcclusionMath::AxisCorners(mn, mx, ahead);
		const float smn[3] = { cam[0] - 2000, cam[1] - 1000, cam[2] - 200 }, smx[3] = { cam[0] + 2000, cam[1] + 3000, cam[2] + 300 };
		OcclusionMath::AxisCorners(smn, smx, straddle);
		const float hmn[3] = { cam[0] - 300, cam[1] - 900, cam[2] - 200 }, hmx[3] = { cam[0] + 300, cam[1] - 100, cam[2] + 200 };
		OcclusionMath::AxisCorners(hmn, hmx, behind);
		CHECK(OcclusionMath::CornersUnclipped(ahead, cam, v, p), "box ahead needs clipping");
		CHECK(!OcclusionMath::CornersUnclipped(straddle, cam, v, p), "box reaching behind the camera needs no clipping");
		OcclusionMath::ScreenVertex cv[OcclusionMath::kMaxClippedVertices], s8[8];
		// Ahead: the clipped path gives the same 6 faces (12 triangles) on the same 8 projected corners.
		const int na = OcclusionMath::ProjectCornersClipped(ahead, cam, v, p, 0, 0, 1920, 1080, 0, 1, 0xFFFFFFFF, cv);
		OcclusionMath::ProjectCornerPoints(ahead, cam, v, p, 0, 0, 1920, 1080, 0, 1, 0xFFFFFFFF, s8);
		CHECK(na == 36, "box ahead clipped into %d vertices, not 36", na);
		bool onCorners = true;
		for (int k = 0; k < na; ++k) {
			bool found = false;
			for (int j = 0; j < 8; ++j)
				found = found || (std::fabs(cv[k].x - s8[j].x) < 0.01f && std::fabs(cv[k].y - s8[j].y) < 0.01f &&
				                  std::fabs(cv[k].z - s8[j].z) < 1e-6f);
			onCorners = onCorners && found;
		}
		CHECK(onCorners, "clipped box ahead has vertices off its corners");
		// Behind: nothing to draw.
		CHECK(OcclusionMath::ProjectCornersClipped(behind, cam, v, p, 0, 0, 1920, 1080, 0, 1, 0xFFFFFFFF, cv) == 0,
			"box behind the camera gave triangles");
		// Straddling (both reversed and standard depth): every vertex in front, within the guard band, depth in range,
		// and every point of the box that is in front and on screen lies inside one of the triangles.
		for (int r = 0; r < 2; ++r) {
			const float* pp = r ? rev : p;
			const int ns = OcclusionMath::ProjectCornersClipped(straddle, cam, v, pp, 0, 0, 1920, 1080, 0, 1, 0xFFFFFFFF, cv);
			CHECK(ns > 0 && ns % 3 == 0 && ns <= OcclusionMath::kMaxClippedVertices, "straddling box: %d vertices", ns);
			bool sane = true;
			for (int k = 0; k < ns; ++k)
				sane = sane && cv[k].rhw <= 1.0f + 1e-6f && cv[k].z >= 0.0f && cv[k].z <= 1.0f && std::fabs(cv[k].x - 960) < 5.0f * 1920 &&
				       std::fabs(cv[k].y - 540) < 5.0f * 1080;
			CHECK(sane, "straddling box: a vertex out of range (%s depth)", r ? "reversed" : "standard");
			auto inTri = [](const OcclusionMath::ScreenVertex& a, const OcclusionMath::ScreenVertex& b,
			                const OcclusionMath::ScreenVertex& c, float x, float y) {
				const float d1 = (x - b.x) * (a.y - b.y) - (a.x - b.x) * (y - b.y);
				const float d2 = (x - c.x) * (b.y - c.y) - (b.x - c.x) * (y - c.y);
				const float d3 = (x - a.x) * (c.y - a.y) - (c.x - a.x) * (y - a.y);
				const float e = 0.5f;
				return !((d1 < -e || d2 < -e || d3 < -e) && (d1 > e || d2 > e || d3 > e));
			};
			int tested = 0, missed = 0;
			unsigned seed = 12345;
			for (int i = 0; i < 4000; ++i) {
				float q[3];
				for (int j = 0; j < 3; ++j) {
					seed = seed * 1664525u + 1013904223u;
					q[j] = smn[j] + (smx[j] - smn[j]) * ((seed >> 8) / 16777216.0f);
				}
				OcclusionMath::Vertex pt = { q[0] - cam[0], q[1] - cam[1], q[2] - cam[2], 0 };
				OcclusionMath::ScreenVertex sp;
				if (!OcclusionMath::ProjectToScreen(&pt, 1, v, pp, 0, 0, 1920, 1080, 0, 1, &sp)) continue;
				if (sp.x < 0 || sp.x > 1919 || sp.y < 0 || sp.y > 1079) continue;
				++tested;
				bool covered = false;
				for (int k = 0; k + 2 < ns && !covered; k += 3) covered = inTri(cv[k], cv[k + 1], cv[k + 2], sp.x, sp.y);
				if (!covered) ++missed;
			}
			CHECK(tested > 100 && missed == 0, "straddling box: %d of %d on-screen points not covered (%s depth)", missed, tested,
				r ? "reversed" : "standard");
		}
	}

	// ProjectBoxForTest: the same vertices as the two separate paths.
	{
		float ahead[8][3], straddle[8][3];
		OcclusionMath::AxisCorners(mn, mx, ahead);
		const float smn[3] = { cam[0] - 2000, cam[1] - 1000, cam[2] - 200 }, smx[3] = { cam[0] + 2000, cam[1] + 3000, cam[2] + 300 };
		OcclusionMath::AxisCorners(smn, smx, straddle);
		OcclusionMath::ScreenVertex a[OcclusionMath::kMaxClippedVertices], b[OcclusionMath::kMaxClippedVertices];
		bool plain = false;
		int n = OcclusionMath::ProjectBoxForTest(ahead, cam, v, p, 0, 0, 1920, 1080, 0, 1, 0xFFFFFFFF, a, plain);
		OcclusionMath::ProjectCornerPoints(ahead, cam, v, p, 0, 0, 1920, 1080, 0, 1, 0xFFFFFFFF, b);
		bool same = n == 8 && plain;
		for (int k = 0; k < 8 && same; ++k) same = a[k].x == b[k].x && a[k].y == b[k].y && a[k].z == b[k].z && a[k].rhw == b[k].rhw;
		CHECK(same, "ProjectBoxForTest (plain) differs from ProjectCornerPoints");
		n = OcclusionMath::ProjectBoxForTest(straddle, cam, v, p, 0, 0, 1920, 1080, 0, 1, 0xFFFFFFFF, a, plain);
		const int m = OcclusionMath::ProjectCornersClipped(straddle, cam, v, p, 0, 0, 1920, 1080, 0, 1, 0xFFFFFFFF, b);
		same = !plain && n == m;
		for (int k = 0; k < n && same; ++k) same = a[k].x == b[k].x && a[k].y == b[k].y && a[k].z == b[k].z;
		CHECK(same, "ProjectBoxForTest (clipped) differs from ProjectCornersClipped");
	}
	// Growth alone can uncover slivers of a box, which is why the plugin draws a grown box together with its original
	// triangles: random boxes (plain and cut), grown by random amounts; every point of the original triangles must lie
	// inside the drawn ones (original + grown), and the grown ones alone must miss somewhere (else the test is moot).
	{
		auto inTri = [](const OcclusionMath::ScreenVertex& a, const OcclusionMath::ScreenVertex& b, const OcclusionMath::ScreenVertex& c,
		                float x, float y) {
			const float d1 = (x - b.x) * (a.y - b.y) - (a.x - b.x) * (y - b.y);
			const float d2 = (x - c.x) * (b.y - c.y) - (b.x - c.x) * (y - c.y);
			const float d3 = (x - a.x) * (c.y - a.y) - (c.x - a.x) * (y - a.y);
			const float e = 1e-3f * (std::fabs(d1) + std::fabs(d2) + std::fabs(d3)) + 0.5f;
			return !((d1 < -e || d2 < -e || d3 < -e) && (d1 > e || d2 > e || d3 > e));
		};
		unsigned seed = 777;
		auto rnd = [&](float lo, float hi) { seed = seed * 1664525u + 1013904223u; return lo + (hi - lo) * ((seed >> 8) / 16777216.0f); };
		int boxes = 0, plainBoxes = 0, missed8 = 0, missedN = 0, grownAloneMissed = 0;
		for (int i = 0; i < 400; ++i) {
			float bmn[3], bmx[3], corners[8][3];
			for (int j = 0; j < 3; ++j) {
				const float cpos = cam[j] + (j == 1 ? rnd(-500, 6000) : rnd(-3000, 3000)), half = rnd(20, 1500);
				bmn[j] = cpos - half; bmx[j] = cpos + half;
			}
			OcclusionMath::AxisCorners(bmn, bmx, corners);
			OcclusionMath::ScreenVertex s[OcclusionMath::kMaxClippedVertices], g[OcclusionMath::kMaxClippedVertices];
			OcclusionMath::ScreenVertex t36[36], g36[36];
			bool plain = false;
			const int n = OcclusionMath::ProjectBoxForTest(corners, cam, v, p, 0, 0, 1920, 1080, 0, 1, 0xFFFFFFFF, s, plain);
			if (n == 0) continue;
			++boxes;
			const float px = rnd(1, 200);
			const OcclusionMath::ScreenVertex* orig;
			const OcclusionMath::ScreenVertex* grown;
			int count;
			if (plain) {
				++plainBoxes;
				for (int k = 0; k < 8; ++k) g[k] = s[k];
				OcclusionMath::GrowOnScreen(g, px);
				OcclusionMath::ExpandBox(s, t36);
				OcclusionMath::ExpandBox(g, g36);
				orig = t36; grown = g36; count = 36;
			} else {
				for (int k = 0; k < n; ++k) g[k] = s[k];
				OcclusionMath::GrowOnScreenN(g, n, px);
				orig = s; grown = g; count = n;
			}
			for (int k = 0; k + 2 < count; k += 3)
				for (int q = 0; q < 6; ++q) {   // points spread over each original triangle
					const float u = rnd(0, 1), w = rnd(0, 1 - u);
					const float x = orig[k].x + u * (orig[k + 1].x - orig[k].x) + w * (orig[k + 2].x - orig[k].x);
					const float y = orig[k].y + u * (orig[k + 1].y - orig[k].y) + w * (orig[k + 2].y - orig[k].y);
					bool covered = false;
					for (int j = 0; j + 2 < count && !covered; j += 3) covered = inTri(grown[j], grown[j + 1], grown[j + 2], x, y);
					if (!covered) ++grownAloneMissed;
					for (int j = 0; j + 2 < count && !covered; j += 3) covered = inTri(orig[j], orig[j + 1], orig[j + 2], x, y);
					if (!covered) ++(plain ? missed8 : missedN);
				}
		}
		CHECK(boxes > 100 && plainBoxes > 20 && boxes - plainBoxes > 20, "growth test: %d boxes (%d plain)", boxes, plainBoxes);
		CHECK(missed8 == 0 && missedN == 0, "original + grown boxes uncovered %d + %d points", missed8, missedN);
		CHECK(grownAloneMissed > 0, "grown boxes alone covered everything: drawing the original too may be unneeded");
	}

	printf(gFails ? "%d FAILED\n" : "all math checks passed\n", gFails);
	return gFails ? 1 : 0;
}
