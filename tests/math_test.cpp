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
	}

	// Rotation angle: identity vs itself 0, vs a 10 degree yaw 10.
	const float I[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
	const float c = std::cos(10 * 0.0174533f), s = std::sin(10 * 0.0174533f);
	const float Z[9] = { c, s, 0, -s, c, 0, 0, 0, 1 };
	CHECK(std::fabs(OcclusionMath::RotationAngleDegrees(I, I)) < 0.05f, "identity angle");
	CHECK(std::fabs(OcclusionMath::RotationAngleDegrees(I, Z) - 10.0f) < 0.05f, "10 degree angle %.3f",
		OcclusionMath::RotationAngleDegrees(I, Z));

	// Distance to box.
	const float bmn[3] = { 0, 0, 0 }, bmx[3] = { 10, 10, 10 };
	const float in[3] = { 5, 5, 5 }, out[3] = { 13, 14, 10 };
	CHECK(OcclusionMath::DistanceToBox(in, bmn, bmx) == 0.0f, "inside");
	CHECK(std::fabs(OcclusionMath::DistanceToBox(out, bmn, bmx) - 5.0f) < 1e-4f, "outside");

	printf(gFails ? "%d FAILED\n" : "all math checks passed\n", gFails);
	return gFails ? 1 : 0;
}
