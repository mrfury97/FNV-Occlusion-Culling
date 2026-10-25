// Occlusion Culling - NVSE
//
// Phase 2: objects the graphics card has shown to be fully covered are not drawn in the main view (their
// BSFadeNode::OnVisible is skipped, exactly as the engine skips an object outside the view). The plugin finds the
// objects the main view draws, asks the card whether each large one is covered by what is already drawn (occlusion
// queries) and hides only those proven covered by recent answers (rules below). A hotkey (iToggleKey) switches the
// hiding on and off while playing; the log keeps measuring either way and compares the frame time of the two states.
//
// How (FalloutNV.exe 1.4.0.525, checked against the decompile and in IDA):
//   - TESMain::DrawScene 0x873200 gathers the main view with BSShaderUtil::AccumulateScene (call at 0x87415B: main
//     camera, world scene, the main culling process) and then draws its opaque part with
//     BSShaderUtil::RenderScenePreResolveDepth (call at 0x874180). Shadows, water reflections, the Pip-Boy, splatter,
//     VATS and menus use other cameras or other calls, so they are never looked at.
//   - Every placed object's root is a BSFadeNode whose pLinkedObj (+0xCC) is its reference. Its OnVisible (vtable slot
//     53, 0x10A9064 in BSFadeNode's vtable, only BSFadeNode itself uses it) runs only when the culler found it inside the
//     view. While the main AccumulateScene runs with the main culler, each top-level fade node is classified there:
//     actors, doors, lights, activators, terminals, projectiles, distant LOD and the like are never candidates; nor is
//     anything small (fMinRadius) or close to the camera (fNearDistance).
//   - Right after the opaque pass the depth buffer holds the finished scene (the engine runs its own VATS query at the
//     same spot). Each candidate's box (union of its shapes' bounds) is drawn there with colour and depth writes off
//     inside a D3D occlusion query, in the renderer's own view and projection (NiDX9Renderer +0x980/+0x9C0) and
//     camera-relative like the engine (NiRenderer::kPosAdjust). Answers are read a frame or more later without waiting.
//   - Hidden: the last two answers drew 0 pixels, the newest is at most iMaxAnswerAge frames old, and the camera has
//     not turned or moved more than the guard allows since (as NVR's HiddenObjectCulling does). A hidden object is still
//     tested every frame, so it comes back the frame after its box shows a pixel. Shadows, reflections and everything
//     outside the main view always draw it.
//   - Every D3D state touched is restored from a state block recorded with exactly those states.
// Hooks chain to whatever was there (NVR's F10 CPU profiler patches the same two calls when switched on, and chains).

#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <unordered_map>
#include <vector>

#include "nvse/nvse_version.h"
#include "nvse/PluginAPI.h"
#include "OcclusionMath.h"
#include "Scheduler.h"

// ---------------------------------------------------------------------------------------------------------------
// Logging and settings

static FILE* gLog = nullptr;

static void Log(const char* fmt, ...) {
	if (!gLog) return;
	va_list args;
	va_start(args, fmt);
	vfprintf(gLog, fmt, args);
	va_end(args);
	fputc('\n', gLog);
	fflush(gLog);
}

struct Settings {
	bool enabled = true;
	float minRadius = 150.0f;        // smaller objects are never tested (game units)
	float nearDistance = 400.0f;     // boxes closer to the camera than this are never tested
	int maxQueries = 500;            // queries issued per frame at most
	int maxAnswerAge = 4;            // oldest answer (frames) that counts
	int visibleRetest = 4;           // a visible object is tested again after this many frames
	float guardDegrees = 2.5f;       // camera turn per frame of answer age beyond which an answer is not used
	float guardUnits = 32.0f;        // camera move per frame of answer age
	int logInterval = 600;           // frames per log block
	bool debugBoxes = false;         // draw the tested boxes (green: visible, red: hidden; red ones through walls)
	bool culling = true;             // start with hiding on
	int toggleKey = 0xD2;            // DirectInput scancode of the on/off key (Insert)
	bool distantLOD = false;         // also test distant LOD objects
} gSet;

static void LoadSettings() {
	char path[MAX_PATH];
	GetFullPathNameA("Data\\NVSE\\Plugins\\OcclusionCulling.ini", MAX_PATH, path, nullptr);
	auto readF = [&](const char* sec, const char* key, float def, float lo, float hi) {
		char b[32];
		GetPrivateProfileStringA(sec, key, "", b, sizeof(b), path);
		if (!b[0]) return def;
		const float v = (float)atof(b);
		return (v >= lo && v <= hi) ? v : def;
	};
	auto readI = [&](const char* sec, const char* key, int def, int lo, int hi) {
		const int v = (int)GetPrivateProfileIntA(sec, key, def, path);
		return (v >= lo && v <= hi) ? v : def;
	};
	gSet.enabled = readI("General", "bEnabled", 1, 0, 1) != 0;
	gSet.minRadius = readF("Candidates", "fMinRadius", 150.0f, 0.0f, 5000.0f);
	gSet.nearDistance = readF("Candidates", "fNearDistance", 400.0f, 50.0f, 10000.0f);
	gSet.maxQueries = readI("Queries", "iMaxQueriesPerFrame", 500, 0, 4000);
	gSet.maxAnswerAge = readI("Queries", "iMaxAnswerAge", 4, 1, 12);
	gSet.visibleRetest = readI("Queries", "iVisibleRetestFrames", 4, 1, 16);
	gSet.guardDegrees = readF("Queries", "fGuardDegreesPerFrame", 2.5f, 0.1f, 45.0f);
	gSet.guardUnits = readF("Queries", "fGuardUnitsPerFrame", 32.0f, 1.0f, 1000.0f);
	gSet.logInterval = readI("Debug", "iLogInterval", 600, 60, 100000);
	gSet.debugBoxes = readI("Debug", "bDebugBoxes", 0, 0, 1) != 0;
	gSet.culling = readI("General", "bCulling", 1, 0, 1) != 0;
	gSet.toggleKey = readI("General", "iToggleKey", 0xD2, 0, 255);
	gSet.distantLOD = readI("Candidates", "bDistantLOD", 0, 0, 1) != 0;
	Log("Settings: enabled %d, hiding %s at start, on/off key 0x%02X | candidates: radius >= %.0f, box farther than %.0f, "
		"distant LOD %d | queries: %d per frame, answers up to %d frames old, visible objects retested every %d frames, guard %.1f deg / "
		"%.0f units per frame | log every %d frames, debug boxes %d", gSet.enabled, gSet.culling ? "on" : "off", gSet.toggleKey,
		gSet.minRadius, gSet.nearDistance, gSet.distantLOD, gSet.maxQueries, gSet.maxAnswerAge, gSet.visibleRetest, gSet.guardDegrees,
		gSet.guardUnits, gSet.logInterval, gSet.debugBoxes);
}

// ---------------------------------------------------------------------------------------------------------------
// Game data

namespace Game {
	constexpr uintptr_t RendererPtr = 0x011C73B4;   // NiDX9Renderer*
	constexpr uintptr_t R_Device = 0x288;           // IDirect3DDevice9*
	constexpr uintptr_t R_DeviceLost = 0x6FC;       // bool m_bDeviceLost
	constexpr uintptr_t R_D3DView = 0x980;          // D3DMATRIX m_kD3DView (camera-relative: see PosAdjust)
	constexpr uintptr_t R_D3DProj = 0x9C0;          // D3DMATRIX m_kD3DProj
	constexpr uintptr_t R_InvView = 0xA40;          // D3DMATRIX m_kInvView (row 4: camera position minus kPosAdjust)
	constexpr uintptr_t PosAdjust = 0x011F474C;     // NiPoint3 NiRenderer::kPosAdjust (subtracted from world positions)
	constexpr uintptr_t PlayerPtr = 0x011DEA3C;     // PlayerCharacter*

	// TESForm / TESObjectREFR
	constexpr uintptr_t Form_Type = 0x04;           // uint8
	constexpr uintptr_t Form_ID = 0x0C;
	constexpr uintptr_t Ref_Base = 0x20;            // TESBoundObject* (kData.pObjectReference)
	constexpr uintptr_t Ref_Position = 0x30;
	constexpr uintptr_t Ref_ParentCell = 0x40;
	constexpr uintptr_t Cell_FullName = 0x1C;       // TESFullName's String: const char*

	// NiAVObject / NiNode / BSFadeNode
	constexpr uintptr_t Ni_WorldBound = 0x20;       // NiBound* (centre xyz, radius)
	constexpr uintptr_t Ni_Flags = 0x30;            // bit 0 APP_CULLED
	constexpr uintptr_t Ni_WorldRotate = 0x68;
	constexpr uintptr_t Ni_WorldTranslate = 0x8C;
	constexpr uintptr_t Ni_WorldScale = 0x98;
	constexpr uintptr_t Node_ChildBase = 0xA0;
	constexpr uintptr_t Node_ChildSize = 0xA6;
	constexpr uintptr_t Fade_MultType = 0xC4;       // ENUM_LOD_MULT
	constexpr uintptr_t Fade_LinkedRef = 0xCC;      // TESObjectREFR* pLinkedObj
	constexpr uintptr_t RTTI_NiNode = 0x011F4428;
	constexpr uintptr_t RTTI_NiGeometry = 0x011F4ACC;
	constexpr int VSlot_GetRTTI = 2;

	enum FormType : uint8_t {
		kACTI = 21, kTACT = 22, kTERM = 23, kDOOR = 28, kLIGH = 30, kPWAT = 35, kCharacter = 59, kCreature = 60,
		kMissile = 61, kGrenade = 62, kBeam = 63, kFlame = 64, kContBeam = 105,
	};
	enum LodMult : uint32_t { kLodActors = 3, kLodDistant = 6, kLodBodyPart = 7, kLodMenu = 8, kLodInvisible = 9, kLodImposter = 10 };

	constexpr uintptr_t Call_MainAccumulate = 0x0087415B;   // TESMain::DrawScene -> BSShaderUtil::AccumulateScene 0xB6BEE0
	constexpr uintptr_t Call_MainOpaque = 0x00874180;       // TESMain::DrawScene -> RenderScenePreResolveDepth 0xB6BA20
	constexpr uintptr_t Vtbl_FadeOnVisible = 0x010A9064;    // BSFadeNode vtable slot 53 -> BSFadeNode::OnVisible 0xB4E490

	constexpr uintptr_t ControlsPtr = 0x011F35CC;   // Controls* (DirectInput state)
	typedef int(__thiscall* GetKeyboardButton_t)(void*, int, int);
	const GetKeyboardButton_t GetKeyboardButton = (GetKeyboardButton_t)0x00A24180;   // Controls::GetKeyboardButton
	constexpr int KeyPressed = 1;                                                    // pressed this frame
	typedef bool(__cdecl* IsInMenuMode_t)();
	const IsInMenuMode_t IsInMenuMode = (IsInMenuMode_t)0x00702360;                  // Interface::IsInMenuMode
	typedef bool(__cdecl* ShowMessage_t)(const char*, int, const char*, const char*, float, bool);
	const ShowMessage_t ShowMessage = (ShowMessage_t)0x007052F0;                     // Interface::ShowMessage (HUD corner)

	template <typename T> T& At(void* base, uintptr_t offset) { return *(T*)((char*)base + offset); }
}

// ---------------------------------------------------------------------------------------------------------------
// Patching

static void SafeWrite32(uintptr_t addr, uint32_t value) {
	DWORD old;
	VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &old);
	*(uint32_t*)addr = value;
	VirtualProtect((void*)addr, 4, old, &old);
}

static uintptr_t ReplaceCall(uintptr_t at, void* fn) {
	if (*(uint8_t*)at != 0xE8) return 0;
	const uintptr_t previous = at + 5 + *(int32_t*)(at + 1);
	SafeWrite32(at + 1, (uintptr_t)fn - at - 5);
	return previous;
}

static uintptr_t ReplaceVirtual(uintptr_t slot, void* fn) {
	const uintptr_t previous = *(uintptr_t*)slot;
	SafeWrite32(slot, (uintptr_t)fn);
	return previous;
}

// ---------------------------------------------------------------------------------------------------------------
// Per-object state

struct Record {
	uint32_t formID = 0;
	float pos[3] = {}, scale = 0.0f;           // identity: a freed node's address reused by another object resets this
	float mn[3] = {}, mx[3] = {};              // box: union of the shapes' world bounds (centre +- radius)
	uint16_t geoms = 0;                        // shapes under the node (draw calls it stands for, at least)
	IDirect3DQuery9* query = nullptr;
	uint32_t issued = 0;                       // frame the pending/last query was drawn in
	bool pending = false;
	bool answered = false;
	uint32_t answerFrame = 0;                  // frame the newest answer was drawn in
	uint32_t pixels = 0;
	uint8_t zeroStreak = 0;                    // consecutive answers with 0 pixels
	uint32_t lastSeen = 0;
};

struct Candidate {
	Record* rec;
	bool wouldHide;
};

static std::unordered_map<void*, Record> gRecords;
static std::vector<Candidate> gCandidates;
static std::vector<IDirect3DQuery9*> gFreeQueries;
static IDirect3DStateBlock9* gState = nullptr;
static bool gQueriesSupported = true;

// Main-pass tracking
static bool gInMain = false;
static void* gMainCuller = nullptr;
static void* gMainCamera = nullptr;
static DWORD gMainThread = 0;
static int gFadeDepth = 0;
static uint32_t gFrame = 1;
static bool gFaulted = false;
static bool gCullingOn = true;      // hiding (the hotkey flips it)
static bool gToggleMessage = false; // show the new state on the HUD after the scene

// Camera per frame (ring), for the answer guard
constexpr int kRing = 16;
static float gCamPos[kRing][3], gCamRot[kRing][9];
static float gNowPos[3], gNowRot[9];

// ---------------------------------------------------------------------------------------------------------------
// Statistics (sums over the log block)

struct Stats {
	double frames = 0;
	double topFade = 0, candidates = 0, noRef = 0, actors = 0, kinds = 0, lod = 0, smallObj = 0, nearSkip = 0, culledFlag = 0;
	double wouldHide = 0, hideGeoms = 0, candGeoms = 0, guardBlocked = 0, tooOld = 0, noAnswer = 0;
	double issued = 0, answered = 0, notReady = 0, failed = 0, budgetSkips = 0, ageSum = 0, zeroAnswers = 0;
	double issuedCovered = 0, issuedNew = 0, issuedVisible = 0, notDue = 0;
	double camMismatch = 0, noDepth = 0, identityResets = 0;
	double hidden = 0, hiddenGeoms = 0;                    // actually not drawn (hiding on)
	double onMs = 0, onFrames = 0, offMs = 0, offFrames = 0;   // frame time while hiding is on / off
	double collectUs = 0, queryUs = 0;
	double records = 0;
} gStats;
static LARGE_INTEGER gQpcFreq;
static bool gDepthGrows = true, gLoggedFirst = false;

static double MicrosSince(const LARGE_INTEGER& start) {
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return (double)(now.QuadPart - start.QuadPart) * 1e6 / (double)gQpcFreq.QuadPart;
}

// ---------------------------------------------------------------------------------------------------------------
// Scene walking

typedef const void*(__thiscall* GetRTTI_t)(void*);

static bool IsKindOf(void* object, uintptr_t rtti) {
	const void* r = (*(GetRTTI_t**)object)[Game::VSlot_GetRTTI](object);
	for (int i = 0; r && i < 16; ++i) {
		if ((uintptr_t)r == rtti) return true;
		r = *(const void* const*)((const char*)r + 4);   // NiRTTI::m_pkBaseRTTI
	}
	return false;
}

struct Shape { float mn[3], mx[3]; int geoms, visited; bool any; };

static void WalkShapes(void* object, int depth, Shape& s) {
	if (!object || depth > 32 || ++s.visited > 4096) return;
	if (Game::At<uint32_t>(object, Game::Ni_Flags) & 1u) return;   // APP_CULLED: not drawn
	if (IsKindOf(object, Game::RTTI_NiNode)) {
		void** children = Game::At<void**>(object, Game::Node_ChildBase);
		const uint16_t count = Game::At<uint16_t>(object, Game::Node_ChildSize);
		for (uint16_t i = 0; children && i < count; ++i) WalkShapes(children[i], depth + 1, s);
	} else if (IsKindOf(object, Game::RTTI_NiGeometry)) {
		++s.geoms;
		const float* b = Game::At<const float*>(object, Game::Ni_WorldBound);
		if (!b || !(b[3] > 0.0f)) return;
		for (int i = 0; i < 3; ++i) {
			s.mn[i] = s.any ? min(s.mn[i], b[i] - b[3]) : b[i] - b[3];
			s.mx[i] = s.any ? max(s.mx[i], b[i] + b[3]) : b[i] + b[3];
		}
		s.any = true;
	}
}

// The box is the union of the shapes' bound cubes, cut to the node's own bound cube (both contain every shape).
static void RebuildShape(void* node, const float* bound, Record& rec) {
	Shape s{};
	WalkShapes(node, 0, s);
	rec.geoms = (uint16_t)min(s.geoms, 65535);
	for (int i = 0; i < 3; ++i) {
		const float lo = bound[i] - bound[3], hi = bound[i] + bound[3];
		rec.mn[i] = s.any ? max(s.mn[i], lo) : lo;
		rec.mx[i] = s.any ? min(s.mx[i], hi) : hi;
		if (rec.mx[i] < rec.mn[i]) { rec.mn[i] = lo; rec.mx[i] = hi; }
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Collecting (main pass, inside BSFadeNode::OnVisible)

// May the newest answer be used now? Its age in frames, and the camera's turn and move since it was drawn.
static bool AnswerUsable(const Record& rec, bool& tooOld) {
	tooOld = false;
	const uint32_t age = gFrame - rec.answerFrame;
	if (age < 1 || age > (uint32_t)gSet.maxAnswerAge || age >= kRing) { tooOld = true; return false; }
	const int slot = rec.answerFrame % kRing;
	const float* p = gCamPos[slot];
	const float dx = gNowPos[0] - p[0], dy = gNowPos[1] - p[1], dz = gNowPos[2] - p[2];
	if (std::sqrt(dx * dx + dy * dy + dz * dz) > gSet.guardUnits * age) return false;
	return OcclusionMath::RotationAngleDegrees(gNowRot, gCamRot[slot]) <= gSet.guardDegrees * age;
}

static __declspec(noinline) bool Collect(void* node) {
	LARGE_INTEGER start;
	QueryPerformanceCounter(&start);
	gStats.topFade += 1;
	void* ref = Game::At<void*>(node, Game::Fade_LinkedRef);
	if (!ref) { gStats.noRef += 1; gStats.collectUs += MicrosSince(start); return false; }
	const uint8_t refType = Game::At<uint8_t>(ref, Game::Form_Type);
	void* base = Game::At<void*>(ref, Game::Ref_Base);
	const uint8_t baseType = base ? Game::At<uint8_t>(base, Game::Form_Type) : 0;
	const uint32_t mult = Game::At<uint32_t>(node, Game::Fade_MultType);
	const float* bound = Game::At<const float*>(node, Game::Ni_WorldBound);

	bool skip = true;
	if (refType == Game::kCharacter || refType == Game::kCreature || mult == Game::kLodActors || mult == Game::kLodBodyPart)
		gStats.actors += 1;
	else if ((refType >= Game::kMissile && refType <= Game::kFlame) || refType == Game::kContBeam || baseType == Game::kDOOR ||
	         baseType == Game::kLIGH || baseType == Game::kACTI || baseType == Game::kTACT || baseType == Game::kTERM ||
	         baseType == Game::kPWAT)
		gStats.kinds += 1;
	else if ((mult == Game::kLodDistant && !gSet.distantLOD) || mult == Game::kLodMenu || mult == Game::kLodInvisible ||
	         mult == Game::kLodImposter)
		gStats.lod += 1;
	else if (!bound || !(bound[3] >= gSet.minRadius))
		gStats.smallObj += 1;
	else if (Game::At<uint32_t>(node, Game::Ni_Flags) & 1u)
		gStats.culledFlag += 1;
	else
		skip = false;
	if (skip) { gStats.collectUs += MicrosSince(start); return false; }

	Record& rec = gRecords[node];
	const uint32_t formID = Game::At<uint32_t>(ref, Game::Form_ID);
	const float* pos = &Game::At<float>(node, Game::Ni_WorldTranslate);
	const float scale = Game::At<float>(node, Game::Ni_WorldScale);
	const bool same = rec.lastSeen != 0 && rec.formID == formID && rec.scale == scale && rec.pos[0] == pos[0] &&
		rec.pos[1] == pos[1] && rec.pos[2] == pos[2];
	if (!same) {
		if (rec.lastSeen != 0) gStats.identityResets += 1;
		if (rec.pending) { rec.query->Release(); rec.query = nullptr; }   // its answer belongs to the old object
		rec.formID = formID; rec.scale = scale;
		rec.pos[0] = pos[0]; rec.pos[1] = pos[1]; rec.pos[2] = pos[2];
		rec.pending = false; rec.answered = false; rec.zeroStreak = 0; rec.pixels = 0;
		RebuildShape(node, bound, rec);
	}
	rec.lastSeen = gFrame;

	if (OcclusionMath::DistanceToBox(gNowPos, rec.mn, rec.mx) < gSet.nearDistance) {
		gStats.nearSkip += 1;
		gStats.collectUs += MicrosSince(start);
		return false;
	}

	gStats.candidates += 1;
	gStats.candGeoms += rec.geoms;
	bool hide = false;
	if (!rec.answered) gStats.noAnswer += 1;
	else if (rec.zeroStreak >= 2) {
		bool tooOld;
		if (AnswerUsable(rec, tooOld)) hide = true;
		else if (tooOld) gStats.tooOld += 1;
		else gStats.guardBlocked += 1;
	}
	if (hide) { gStats.wouldHide += 1; gStats.hideGeoms += rec.geoms; }
	gCandidates.push_back({ &rec, hide });
	if (hide && gCullingOn) { gStats.hidden += 1; gStats.hiddenGeoms += rec.geoms; }
	gStats.collectUs += MicrosSince(start);
	return hide;
}

static void OnFault(const char* where) {
	gFaulted = true;
	Log("Error: an exception in %s; the plugin has switched itself off for this session", where);
}

// True when the object is to be hidden. An exception draws it (and switches the plugin off).
static bool SafeCollect(void* node) {
	__try { return Collect(node); }
	__except (EXCEPTION_EXECUTE_HANDLER) { OnFault("collecting"); return false; }
}

// ---------------------------------------------------------------------------------------------------------------
// Queries (after the main opaque pass)

static void ReleaseAll() {
	for (auto& entry : gRecords) if (entry.second.query) entry.second.query->Release();
	gRecords.clear();
	for (IDirect3DQuery9* q : gFreeQueries) q->Release();
	gFreeQueries.clear();
	if (gState) { gState->Release(); gState = nullptr; }
	gCandidates.clear();
}

static constexpr DWORD kFVF = D3DFVF_XYZ | D3DFVF_DIFFUSE;

static void SetQueryStates(IDirect3DDevice9* device, const D3DMATRIX& view, const D3DMATRIX& proj) {
	D3DMATRIX identity = {};
	identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
	device->SetVertexShader(nullptr);
	device->SetPixelShader(nullptr);
	device->SetFVF(kFVF);
	device->SetTexture(0, nullptr);
	device->SetTransform(D3DTS_WORLD, &identity);
	device->SetTransform(D3DTS_VIEW, &view);
	device->SetTransform(D3DTS_PROJECTION, &proj);
	device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
	device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	device->SetRenderState(D3DRS_ZFUNC, gDepthGrows ? D3DCMP_LESSEQUAL : D3DCMP_GREATEREQUAL);
	device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
	device->SetRenderState(D3DRS_COLORWRITEENABLE1, 0);
	device->SetRenderState(D3DRS_COLORWRITEENABLE2, 0);
	device->SetRenderState(D3DRS_COLORWRITEENABLE3, 0);
	device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	device->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
	device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
	device->SetRenderState(D3DRS_FOGENABLE, FALSE);
	device->SetRenderState(D3DRS_LIGHTING, FALSE);
	device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
	device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
	device->SetRenderState(D3DRS_DEPTHBIAS, 0);
	device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
	device->SetRenderState(D3DRS_CLIPPING, TRUE);
	device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
	device->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
	device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
	device->SetRenderState(D3DRS_COLORVERTEX, TRUE);
	device->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, FALSE);
	device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
	device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
	device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

// A state block holding exactly the states drawn with below; Capture() before, Apply() after. It is recorded inside a
// full snapshot so the recording itself cannot leave anything changed, whatever the driver does while recording.
static bool EnsureStateBlock(IDirect3DDevice9* device) {
	if (gState) return true;
	IDirect3DStateBlock9* all = nullptr;
	if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &all)) || !all) return false;
	bool ok = SUCCEEDED(device->BeginStateBlock());
	if (ok) {
		D3DMATRIX identity = {};
		identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
		SetQueryStates(device, identity, identity);   // the same calls as each frame, with valid values
		device->SetStreamSource(0, nullptr, 0, 0);    // DrawPrimitiveUP clears stream 0; the block puts it back
		ok = SUCCEEDED(device->EndStateBlock(&gState)) && gState;
	}
	all->Apply();
	all->Release();
	if (!ok) Log("Could not record the state block: queries off");
	return ok;
}

static IDirect3DQuery9* AcquireQuery(IDirect3DDevice9* device) {
	if (!gFreeQueries.empty()) { IDirect3DQuery9* q = gFreeQueries.back(); gFreeQueries.pop_back(); return q; }
	IDirect3DQuery9* q = nullptr;
	if (FAILED(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &q))) return nullptr;
	return q;
}

static __declspec(noinline) void RunQueries(void* camera) {
	char* renderer = *(char**)Game::RendererPtr;
	if (!renderer) return;
	IDirect3DDevice9* device = *(IDirect3DDevice9**)(renderer + Game::R_Device);
	if (!device) return;
	if (Game::At<bool>(renderer, Game::R_DeviceLost)) { ReleaseAll(); return; }

	if (!gLoggedFirst) {
		gLoggedFirst = true;
		gQueriesSupported = SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, nullptr));
		gDepthGrows = OcclusionMath::DepthGrowsWithDistance(&Game::At<float>(renderer, Game::R_D3DProj));
		Log("First frame: occlusion queries %s, depth test %s", gQueriesSupported ? "supported" : "NOT supported",
			gDepthGrows ? "LESSEQUAL (standard depth)" : "GREATEREQUAL (reversed depth)");
	}
	if (!gQueriesSupported) return;

	// 1. Answers of earlier frames.
	for (auto& entry : gRecords) {
		Record& rec = entry.second;
		if (!rec.pending) continue;
		DWORD pixels = 0;
		const HRESULT hr = rec.query->GetData(&pixels, sizeof(pixels), 0);
		if (hr == S_OK) {
			rec.pending = false;
			rec.answered = true;
			rec.answerFrame = rec.issued;
			rec.pixels = pixels;
			rec.zeroStreak = pixels ? 0 : (uint8_t)min(rec.zeroStreak + 1, 255);
			gStats.answered += 1;
			gStats.ageSum += gFrame - rec.issued;
			if (!pixels) gStats.zeroAnswers += 1;
		} else if (hr == S_FALSE) {
			gStats.notReady += 1;
		} else {
			gStats.failed += 1;
			if (hr == D3DERR_DEVICELOST) { ReleaseAll(); return; }
			rec.pending = false;
			rec.query->Release();   // never reused: after a device reset it may be dead
			rec.query = nullptr;
		}
	}

	// 2. This frame's queries, only when the renderer's camera is the main camera and a depth buffer is bound.
	const float* posAdjust = (const float*)Game::PosAdjust;
	const D3DMATRIX& view = Game::At<D3DMATRIX>(renderer, Game::R_D3DView);
	const D3DMATRIX& proj = Game::At<D3DMATRIX>(renderer, Game::R_D3DProj);
	const D3DMATRIX& invView = Game::At<D3DMATRIX>(renderer, Game::R_InvView);
	const float* camPos = &Game::At<float>(camera, Game::Ni_WorldTranslate);
	const float ex = posAdjust[0] + invView._41 - camPos[0], ey = posAdjust[1] + invView._42 - camPos[1],
		ez = posAdjust[2] + invView._43 - camPos[2];
	bool canIssue = ex * ex + ey * ey + ez * ez < 4.0f;
	if (!canIssue) gStats.camMismatch += 1;
	IDirect3DSurface9* depth = nullptr;
	if (canIssue && (FAILED(device->GetDepthStencilSurface(&depth)) || !depth)) { canIssue = false; gStats.noDepth += 1; }
	if (depth) depth->Release();

	if (canIssue && !gCandidates.empty() && EnsureStateBlock(device)) {
		gDepthGrows = OcclusionMath::DepthGrowsWithDistance(&proj._11);
		gState->Capture();
		SetQueryStates(device, view, proj);
		// Who is tested this frame, in what order (Scheduler.h).
		static std::vector<Scheduler::Item> order;
		order.clear();
		for (uint32_t i = 0; i < (uint32_t)gCandidates.size(); ++i) {
			const Record& rec = *gCandidates[i].rec;
			if (rec.pending) continue;
			const uint32_t since = gFrame - rec.issued;
			const Scheduler::Class cls = Scheduler::Classify(rec.answered, rec.zeroStreak, since, (uint32_t)gSet.visibleRetest);
			if (cls == Scheduler::kNotDue) { gStats.notDue += 1; continue; }
			order.push_back({ (uint8_t)cls, since, i });
		}
		Scheduler::Order(order);

		OcclusionMath::Vertex box[36];
		int issued = 0;
		for (const Scheduler::Item& item : order) {
			if (issued >= gSet.maxQueries) { gStats.budgetSkips += 1; continue; }
			Record& rec = *gCandidates[item.index].rec;
			if (!rec.query) rec.query = AcquireQuery(device);
			if (!rec.query) break;
			OcclusionMath::BoxTriangles(rec.mn, rec.mx, posAdjust, 0xFFFFFFFF, box);
			rec.query->Issue(D3DISSUE_BEGIN);
			device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 12, box, sizeof(OcclusionMath::Vertex));
			rec.query->Issue(D3DISSUE_END);
			rec.pending = true;
			rec.issued = gFrame;
			++issued;
			if (item.cls == Scheduler::kCovered) gStats.issuedCovered += 1;
			else if (item.cls == Scheduler::kNew) gStats.issuedNew += 1;
			else gStats.issuedVisible += 1;
		}
		gStats.issued += issued;

		if (gSet.debugBoxes) {
			device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
			device->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
			for (const Candidate& c : gCandidates) {
				const Record& rec = *c.rec;
				if (!rec.answered) continue;
				device->SetRenderState(D3DRS_ZENABLE, c.wouldHide ? D3DZB_FALSE : D3DZB_TRUE);
				OcclusionMath::BoxTriangles(rec.mn, rec.mx, posAdjust, c.wouldHide ? 0xFFFF2020 : 0xFF20FF20, box);
				device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 12, box, sizeof(OcclusionMath::Vertex));
			}
		}
		gState->Apply();
	}

	// 3. Forget objects not drawn for a while.
	if (gFrame % 64 == 0) {
		for (auto it = gRecords.begin(); it != gRecords.end();) {
			if (gFrame - it->second.lastSeen > 600) {
				if (it->second.query) {
					if (it->second.pending) it->second.query->Release();
					else gFreeQueries.push_back(it->second.query);
				}
				it = gRecords.erase(it);
			} else ++it;
		}
	}
	gStats.records += (double)gRecords.size();
}

static void SafeRunQueries(void* camera) {
	__try { RunQueries(camera); }
	__except (EXCEPTION_EXECUTE_HANDLER) {
		if (gState) gState->Apply();
		OnFault("the query pass");
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Log block

static void LogBlock() {
	const double f = gStats.frames > 0 ? gStats.frames : 1;
	void* pc = *(void**)Game::PlayerPtr;
	float px = 0, py = 0, pz = 0;
	uint32_t cellID = 0;
	const char* cellName = nullptr;
	if (pc) {
		px = Game::At<float>(pc, Game::Ref_Position); py = Game::At<float>(pc, Game::Ref_Position + 4);
		pz = Game::At<float>(pc, Game::Ref_Position + 8);
		void* cell = Game::At<void*>(pc, Game::Ref_ParentCell);
		if (cell) { cellID = Game::At<uint32_t>(cell, Game::Form_ID); cellName = Game::At<const char*>(cell, Game::Cell_FullName); }
	}
	char name[64] = "";
	if (cellName && !IsBadStringPtrA(cellName, 63)) strncpy_s(name, cellName, _TRUNCATE);
	const double cand = gStats.candidates / f;
	Log("--- %.0f frames, player at (%.0f, %.0f, %.0f) in cell %08X \"%s\"", gStats.frames, px, py, pz, cellID, name);
	Log("  objects drawn by the main view (per frame): %.0f; not candidates: %.1f no reference, %.1f actors, %.1f doors/lights/"
		"activators/effects, %.1f distant LOD, %.1f small, %.1f hidden by the game, %.1f near the camera", gStats.topFade / f,
		gStats.noRef / f, gStats.actors / f, gStats.kinds / f, gStats.lod / f, gStats.smallObj / f, gStats.culledFlag / f,
		gStats.nearSkip / f);
	if (gStats.topFade == 0)
		Log("  Warning: no object reached the BSFadeNode hook in this block (another plugin replaced that vtable slot later?)");
	Log("  hiding %s: %.1f objects = %.0f shapes not drawn per frame | frame time: %.2f ms with hiding on (%.0f frames), %.2f ms "
		"with it off (%.0f frames)", gCullingOn ? "ON" : "OFF", gStats.hidden / f, gStats.hiddenGeoms / f,
		gStats.onFrames > 0 ? gStats.onMs / gStats.onFrames : 0.0, gStats.onFrames,
		gStats.offFrames > 0 ? gStats.offMs / gStats.offFrames : 0.0, gStats.offFrames);
	Log("  candidates: %.1f per frame (%.0f shapes); covered %.1f (%.1f%%) = %.0f shapes (%.1f%% of candidate shapes)",
		cand, gStats.candGeoms / f, gStats.wouldHide / f, cand > 0 ? 100.0 * gStats.wouldHide / gStats.candidates : 0.0,
		gStats.hideGeoms / f, gStats.candGeoms > 0 ? 100.0 * gStats.hideGeoms / gStats.candGeoms : 0.0);
	Log("  covered but not used: %.1f camera guard, %.1f answer too old; %.1f with no answer yet", gStats.guardBlocked / f,
		gStats.tooOld / f, gStats.noAnswer / f);
	Log("  queries per frame: %.1f issued (%.1f covered, %.1f new, %.1f visible; %.1f visible waiting their turn), %.1f answered "
		"(%.1f zero pixels), average age %.2f frames, %.1f not ready, %.2f failed, %.1f over budget | records %.0f",
		gStats.issued / f, gStats.issuedCovered / f, gStats.issuedNew / f, gStats.issuedVisible / f, gStats.notDue / f,
		gStats.answered / f, gStats.zeroAnswers / f,
		gStats.answered > 0 ? gStats.ageSum / gStats.answered : 0.0, gStats.notReady / f, gStats.failed / f, gStats.budgetSkips / f,
		gStats.records / f);
	Log("  cost per frame: %.0f us collecting, %.0f us queries (CPU) | frames skipped: %.0f camera mismatch, %.0f no depth "
		"buffer | identity resets %.0f", gStats.collectUs / f, gStats.queryUs / f, gStats.camMismatch, gStats.noDepth,
		gStats.identityResets);
	gStats = Stats{};
}

// ---------------------------------------------------------------------------------------------------------------
// Hooks

typedef void(__cdecl* Accumulate_t)(void*, void*, void*);
static Accumulate_t OrigAccumulate = nullptr;
typedef void(__cdecl* Opaque_t)(void*, void*);
static Opaque_t OrigOpaque = nullptr;
typedef void(__thiscall* OnVisible_t)(void*, void*);
static OnVisible_t OrigFadeOnVisible = nullptr;

static void __cdecl AccumulateHook(void* camera, void* scene, void* culler) {
	const bool active = gSet.enabled && !gFaulted && camera && GetCurrentThreadId() == gMainThread;
	if (active) {
		gCandidates.clear();
		const float* p = &Game::At<float>(camera, Game::Ni_WorldTranslate);
		const float* r = &Game::At<float>(camera, Game::Ni_WorldRotate);
		for (int i = 0; i < 3; ++i) gNowPos[i] = p[i];
		for (int i = 0; i < 9; ++i) gNowRot[i] = r[i];
		gMainCuller = culler;
		gMainCamera = camera;
		gFadeDepth = 0;
		gInMain = true;
	}
	OrigAccumulate(camera, scene, culler);
	gInMain = false;
}

static void __fastcall FadeOnVisibleHook(void* node, void*, void* culler) {
	if (gInMain && culler == gMainCuller && GetCurrentThreadId() == gMainThread) {
		if (gFadeDepth == 0 && SafeCollect(node) && gCullingOn && !gFaulted) return;   // proven covered: not drawn
		++gFadeDepth;
		OrigFadeOnVisible(node, culler);
		--gFadeDepth;
		return;
	}
	OrigFadeOnVisible(node, culler);
}

static unsigned gOpaqueCalls = 0;

static void __cdecl OpaqueHook(void* camera, void* accumulator) {
	OrigOpaque(camera, accumulator);
	if (!gSet.enabled || gFaulted) return;
	if (++gOpaqueCalls == 600 && gFrame == 1)
		Log("Warning: 600 main opaque passes and none followed the hooked main accumulate (multithreaded rendering on, or "
			"the camera differs): nothing is measured");
	if (!camera || camera != gMainCamera || GetCurrentThreadId() != gMainThread) return;
	LARGE_INTEGER start;
	QueryPerformanceCounter(&start);

	// Frame time between main views, credited to the state the frame was drawn in (pauses and loads left out).
	static LARGE_INTEGER last = {};
	if (last.QuadPart) {
		const double ms = (double)(start.QuadPart - last.QuadPart) * 1000.0 / (double)gQpcFreq.QuadPart;
		if (ms > 0.0 && ms < 250.0) {
			if (gCullingOn) { gStats.onMs += ms; gStats.onFrames += 1; }
			else { gStats.offMs += ms; gStats.offFrames += 1; }
		}
	}
	last = start;

	// The on/off key (not while a menu or the console takes the keyboard).
	void* controls = *(void**)Game::ControlsPtr;
	if (gSet.toggleKey && controls && !Game::IsInMenuMode() &&
		Game::GetKeyboardButton(controls, gSet.toggleKey, Game::KeyPressed)) {
		gCullingOn = !gCullingOn;
		gToggleMessage = true;
		Log("[frame %u] hiding switched %s with the key", gFrame, gCullingOn ? "ON" : "OFF");
	}
	SafeRunQueries(camera);
	gStats.queryUs += MicrosSince(start);
	gCandidates.clear();
	gMainCamera = nullptr;   // one query pass per main accumulate

	const int slot = gFrame % kRing;
	for (int i = 0; i < 3; ++i) gCamPos[slot][i] = gNowPos[i];
	for (int i = 0; i < 9; ++i) gCamRot[slot][i] = gNowRot[i];
	++gFrame;
	gStats.frames += 1;
	if (gStats.frames >= gSet.logInterval) LogBlock();
	if (gToggleMessage) {
		gToggleMessage = false;
		Game::ShowMessage(gCullingOn ? "Occlusion culling: ON" : "Occlusion culling: OFF (measuring only)", 0, nullptr, nullptr,
			2.0f, true);
	}
}

// ---------------------------------------------------------------------------------------------------------------
// NVSE

static void MessageHandler(NVSEMessagingInterface::Message* msg) {
	if (msg->type != NVSEMessagingInterface::kMessage_DeferredInit) return;
	if (!gSet.enabled) { Log("Disabled (bEnabled=0): no hooks"); return; }
	gMainThread = GetCurrentThreadId();
	OrigAccumulate = (Accumulate_t)ReplaceCall(Game::Call_MainAccumulate, &AccumulateHook);
	if (!OrigAccumulate) { Log("0x%08X is not a call: disabled", (unsigned)Game::Call_MainAccumulate); return; }
	OrigOpaque = (Opaque_t)ReplaceCall(Game::Call_MainOpaque, &OpaqueHook);
	if (!OrigOpaque) {
		ReplaceCall(Game::Call_MainAccumulate, (void*)OrigAccumulate);   // put the first one back
		OrigAccumulate = nullptr;
		Log("0x%08X is not a call: disabled", (unsigned)Game::Call_MainOpaque);
		return;
	}
	OrigFadeOnVisible = (OnVisible_t)ReplaceVirtual(Game::Vtbl_FadeOnVisible, &FadeOnVisibleHook);
	Log("Hooks installed: main accumulate -> 0x%08X, main opaque pass -> 0x%08X, BSFadeNode::OnVisible -> 0x%08X",
		(unsigned)(uintptr_t)OrigAccumulate, (unsigned)(uintptr_t)OrigOpaque, (unsigned)(uintptr_t)OrigFadeOnVisible);
	gCullingOn = gSet.culling;
	Log("Hiding starts %s; key 0x%02X switches it. A block of numbers is logged every %d frames of world view.",
		gCullingOn ? "ON" : "OFF (measuring only)", gSet.toggleKey, gSet.logInterval);
}

extern "C" __declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info) {
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "Occlusion Culling";
	info->version = 1;
	return !nvse->isEditor && nvse->runtimeVersion == RUNTIME_VERSION_1_4_0_525;
}

extern "C" __declspec(dllexport) bool NVSEPlugin_Load(NVSEInterface* nvse) {
	CreateDirectoryA("logs", nullptr);
	gLog = _fsopen("logs\\OcclusionCulling.log", "w", _SH_DENYWR);
	Log("Occlusion Culling 0.2.1 (phase 2: hiding covered objects, hotkey on/off; covered objects tested first)");
	QueryPerformanceFrequency(&gQpcFreq);
	LoadSettings();
	auto* messaging = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
	if (!messaging || !messaging->RegisterListener(nvse->GetPluginHandle(), "NVSE", MessageHandler))
		Log("No NVSE messaging: disabled");
	return true;
}

BOOL WINAPI DllMain(HANDLE, DWORD, LPVOID) { return TRUE; }
