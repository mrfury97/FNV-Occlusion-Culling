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
//   - The boxes are projected on the CPU (pre-transformed XYZRHW vertices), so no transform or vertex-pipeline state is
//     touched. Only colour writes, depth write/test, culling, scissor/stencil (when on), the shaders, the vertex format and
//     stream 0 are changed, each read first and set back to exactly that value. No state blocks: re-sending states (a
//     D3DSBT_ALL snapshot, or toggling alpha testing) disturbed NVIDIA's alpha-to-coverage driver hack that the game uses
//     with bTransparencyMultisampling and MSAA, and left a dithered speckle on most surfaces until the game restarted.
//     Alpha testing is never touched. When it is on at that point, the boxes are drawn fully opaque (vertex alpha 255,
//     stage 0 alpha from the vertex colour, stage 1 off, all put back after) so they pass it; only a comparison that would
//     cut opaque pixels (LESS, NEVER, ...) still skips the frame.
// Hooks chain to whatever was there (NVR's F10 CPU profiler patches the same two calls when switched on, and chains).
//
// Tighter boxes ([Boxes] bOrientedBoxes): each shape's own vertices (NiGeometryData m_pkVertex, model space) give its
// exact extent, measured once per mesh and cached. The object's box is then built twice, in the object's own rotated
// frame (an oriented box: a wall placed at 30 degrees gets a box along the wall) and along the world axes, and the
// smaller of these and the old box is used. Every box holds the whole object, so answers stay valid when it changes.
// Shapes whose vertices the game has freed after upload, skinned shapes and particles keep their bound spheres;
// objects with animation controllers keep the old box. iCompareKey switches actors or oriented boxes off, one at a time,
// for comparisons (0.5.0 also had grouped tests, one query for several covered objects: removed in 0.7.0, no gain).
//
// Actors ([Actors] bActorCulling): NPCs and creatures (not the player, not severed limbs) are tested too. Skipping an
// actor's BSFadeNode::OnVisible is exactly what the engine does when the actor is outside the view: fade, animation, AI
// and BSFadeNode::IsVisible do not depend on it (checked in the decompile). An actor moves, so its box is rebuilt every
// frame from the root's world bound (the sphere the engine itself culls with) plus a margin (10% + fActorMargin), and it
// is hidden only while its current bound still lies inside the box its newest 0-pixel answer was for. The margin
// shows pixels before the actor itself steps out from behind a wall.
//
// Doors, activators, talking activators and terminals ([Activators] bActivatorCulling): they can open, swing or
// animate, so they take the actors' path: box = the root's world bound (updated by the engine every frame, animation
// included) plus 5% + fActivatorMargin, rebuilt every frame, hidden only while the current bound is still inside the
// box of the newest 0-pixel answer. Ones with no shapes (trigger boxes and the like) are never tested.
//
// SpeedTree trees and shrubs ([Trees] bTreeCulling): their root is a BSTreeNode, which has its own OnVisible (vtable slot
// 53 at 0x10669B8 -> BSTreeNode::OnVisible 0x66BF50: only NiNode::OnVisible when trees are on), so they never reach the
// BSFadeNode hook. Branches, leaves and the billboard are its children, so skipping it skips the whole tree, exactly as
// when it is outside the view. Leaf cards are stored as their centre points and spread out in the vertex shader
// (BSTreeModel::CreateLeafGeometry), so a tree's bound does not hold its outer leaves: its box is the bound's cube plus
// 20% of the radius plus fTreeMargin. FalloutNV.esm places about 18,400 of them (WastelandShrub01, EuonymusBush01,
// WhiteOak01); Joshua trees and most dead trees are ordinary statics.
//
// (0.8.1 drew all of a frame's boxes from one vertex buffer; it saved about 5% of the drawing time, 30 us at 1000
// tests, for a buffer that had to be released before every device reset, so 0.8.2 went back to one draw per box.)
//
// Growing boxes while moving ([Queries] fRevealDepth): an answer is one frame old when it is used, so when the camera
// moves, an object behind an occluder's edge can come into view a frame before its own test says so (pop-in when
// stepping out from a wall). Rotation moves occluder and object together; a sideways move slides near edges across the
// screen faster than what lies behind them. So every frame the boxes of things whose last answer was 0 pixels grow on
// screen by how far the camera's move this frame slides an edge fRevealDepth units away (OcclusionMath::RevealPixels),
// and they show pixels a frame early instead. Standing still nothing grows; a lower fRevealDepth grows more.
// Moves under kRevealMinMove units a frame (idle sway, creeping) grow nothing. When a grown box shows pixels, the object
// is drawn but tested again the very next frame, and one 0-pixel answer puts back the covered state it had before (a
// grown box's pixels need not be the object's own), so an early reveal costs about two frames of drawing, not six.
//
// Hidden lamps ([Lamps]): every frame ShadowSceneNode::PreOnVisible runs ShadowSceneLight::TestFrustumCull (call at
// 0xB5E915, also patched by NVR's KeepLampsInView, which we chain) on each lamp; usFrustumCull (+0x110) = 255 switches
// the lamp off for that frame, and the game marks the shaders of every shape the lamp lights (kGeometries +0xE0,
// bLightListChanged) to rebuild their lamp passes when that changes. Each lamp's reach (a box around its radius,
// NiLight m_kSpec.r) is tested with the same occlusion queries: when no visible pixel lies inside it, nothing the lamp
// could light is on screen. Measured always; with bLampCulling=1 such a lamp is also switched off for the main view
// (after three 0-pixel answers, the camera guard as for objects, never within fNearDistance of the camera), with the
// value the game and NVR left restored before each next test and the lit shapes marked to rebuild when ours changes.

#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

// Release builds (compiled with /DOC_RELEASE, tools\build_release.bat) leave out the measuring tools: the log blocks
// of numbers and their timers, the debug boxes and the compare key. The log then holds the start-up lines, the
// settings, the on/off key and any error. Developer builds keep everything.
#ifdef OC_RELEASE
constexpr bool kDevTools = false;
#define OC_BUILD "release build"
#else
constexpr bool kDevTools = true;
#define OC_BUILD "developer build"
#endif
constexpr const char* kVersion = "1.1.0-dev10";

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
	float revealDepth = 150.0f;      // boxes grow by how far a camera move slides an edge this far away (0 = off)
	int logInterval = 600;           // frames per log block
	bool debugBoxes = false;         // draw the tested boxes (green: visible, red: hidden; red ones through walls)
	bool culling = true;             // start with hiding on
	int toggleKey = 0xD2;            // DirectInput scancode of the on/off key (Insert)
	bool distantLOD = false;         // also test distant LOD objects
	bool imposters = true;           // also test imposters (refs flagged to show at any distance: never faded out)
	bool lampCulling = false;        // switch hidden lamps off (otherwise they are only measured)
	bool orientedBoxes = true;       // tighter boxes from the shapes' vertices, in the object's own frame
	int compareKey = 0xCF;           // switches actors, then oriented boxes, off for comparisons (End)
	bool actorCulling = true;        // NPCs and creatures are tested and hidden too
	float actorMargin = 24.0f;       // added around an actor's bound (plus 10% of its radius)
	bool animThrottle = true;        // actors the main view does not draw update their bones every animInterval frames
	int animInterval = 2;
	float animNearDistance = 512.0f; // actors closer than this to the camera always update every frame
	bool activatorCulling = true;    // doors, activators, talking activators and terminals are tested and hidden too
	float activatorMargin = 16.0f;   // added around their bound (plus 5% of its radius)
	bool treeCulling = true;         // SpeedTree trees and shrubs are tested and hidden too
	bool landCulling = true;         // land quadrants (2048 x 2048 units, four per cell) are tested and hidden too
	float landMargin = 32.0f;        // added above and below a quadrant's height range (tessellated land moves)
	bool lodTerrain = true;          // distant terrain LOD chunks (under the scene's LandLOD node) are tested and hidden too
	bool grassCulling = true;        // grass groups (TallGrassTriStrips under the scene's Grass node) are tested and hidden too
	float grassMargin = 32.0f;       // added around a grass group's box on every side (blades sway in the wind)
	float treeMargin = 32.0f;        // added around a tree's bound (plus 20% of its radius) for its leaf cards
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
	gSet.revealDepth = readF("Queries", "fRevealDepth", 150.0f, 0.0f, 100000.0f);
	gSet.logInterval = readI("Debug", "iLogInterval", 600, 60, 100000);
	gSet.debugBoxes = kDevTools && readI("Debug", "bDebugBoxes", 0, 0, 1) != 0;
	gSet.culling = readI("General", "bCulling", 1, 0, 1) != 0;
	gSet.toggleKey = readI("General", "iToggleKey", 0xD2, 0, 255);
	gSet.distantLOD = readI("Candidates", "bDistantLOD", 0, 0, 1) != 0;
	gSet.imposters = readI("Candidates", "bImposters", 1, 0, 1) != 0;
	gSet.lampCulling = readI("Lamps", "bLampCulling", 0, 0, 1) != 0;
	gSet.orientedBoxes = readI("Boxes", "bOrientedBoxes", 1, 0, 1) != 0;
	gSet.compareKey = kDevTools ? readI("Debug", "iCompareKey", 0xCF, 0, 255) : 0;
	gSet.actorCulling = readI("Actors", "bActorCulling", 1, 0, 1) != 0;
	gSet.actorMargin = readF("Actors", "fActorMargin", 24.0f, 0.0f, 512.0f);
	gSet.animThrottle = readI("Actors", "bAnimThrottle", 1, 0, 1) != 0;
	gSet.animInterval = readI("Actors", "iAnimInterval", 2, 1, 8);
	gSet.animNearDistance = readF("Actors", "fAnimNearDistance", 512.0f, 0.0f, 100000.0f);
	gSet.activatorCulling = readI("Activators", "bActivatorCulling", 1, 0, 1) != 0;
	gSet.activatorMargin = readF("Activators", "fActivatorMargin", 16.0f, 0.0f, 512.0f);
	Log("Doors, activators, terminals: %s, margin 5%% + %.0f units",
		gSet.activatorCulling ? "tested and hidden like actors" : "never touched (bActivatorCulling=0)", gSet.activatorMargin);
	gSet.treeCulling = readI("Trees", "bTreeCulling", 1, 0, 1) != 0;
	gSet.treeMargin = readF("Trees", "fTreeMargin", 32.0f, 0.0f, 1024.0f);
	gSet.landCulling = readI("Land", "bLandCulling", 1, 0, 1) != 0;
	gSet.landMargin = readF("Land", "fLandMargin", 32.0f, 0.0f, 1024.0f);
	gSet.lodTerrain = readI("Land", "bTerrainLOD", 1, 0, 1) != 0;
	gSet.grassCulling = readI("Grass", "bGrassCulling", 1, 0, 1) != 0;
	gSet.grassMargin = readF("Grass", "fGrassMargin", 32.0f, 0.0f, 1024.0f);
	Log("Trees: %s, margin 20%% + %.0f units", gSet.treeCulling ? "tested and hidden like objects" : "never touched (bTreeCulling=0)",
		gSet.treeMargin);
	Log("Actors: %s, margin 10%% + %.0f units", gSet.actorCulling ? "tested and hidden like objects" : "never touched (bActorCulling=0)",
		gSet.actorMargin);
	if (gSet.animThrottle && gSet.animInterval > 1)
		Log("Actor bones: actors behind cover or outside the view, %.0f+ units away, update every %d frames", gSet.animNearDistance,
			gSet.animInterval);
	else
		Log("Actor bones: every frame for all actors (bAnimThrottle=0 or iAnimInterval=1)");
	Log("Land: %s, height margin %.0f units", gSet.landCulling ? "quadrants tested and hidden like objects" :
		"never touched (bLandCulling=0)", gSet.landMargin);
	Log("Distant terrain LOD: %s", gSet.lodTerrain ? "chunks tested and hidden like land" : "never touched (bTerrainLOD=0)");
	Log("Grass: %s, margin %.0f units", gSet.grassCulling ? "groups tested and hidden like land" : "never touched (bGrassCulling=0)",
		gSet.grassMargin);
	Log("Lamps: hidden lamps %s", gSet.lampCulling ? "switched off in the main view" : "measured only (bLampCulling=0)");
	if (kDevTools) Log("Boxes: oriented %d | compare key 0x%02X", gSet.orientedBoxes, gSet.compareKey);
	else Log("Boxes: oriented %d | boxes grow while moving: fRevealDepth %.0f", gSet.orientedBoxes, gSet.revealDepth);
	Log("Settings: enabled %d, hiding %s at start, on/off key 0x%02X | candidates: radius >= %.0f, box farther than %.0f, "
		"distant LOD %d, imposters %d | queries: %d per frame, answers up to %d frames old, visible objects retested every %d frames, guard %.1f deg / "
		"%.0f units per frame | log every %d frames, debug boxes %d", gSet.enabled, gSet.culling ? "on" : "off", gSet.toggleKey,
		gSet.minRadius, gSet.nearDistance, gSet.distantLOD, gSet.imposters, gSet.maxQueries, gSet.maxAnswerAge, gSet.visibleRetest, gSet.guardDegrees,
		gSet.guardUnits, kDevTools ? gSet.logInterval : 0, gSet.debugBoxes);
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
	constexpr uintptr_t RTTI_NiTriBasedGeom = 0x011F5B70;   // NiTriShape, NiTriStrips and the BS shapes derived from them
	constexpr uintptr_t Obj_Controllers = 0x0C;     // NiObjectNET::m_spControllers
	constexpr uintptr_t Geo_ModelData = 0xB8;       // NiGeometry::m_spModelData (NiGeometryData*)
	constexpr uintptr_t Geo_SkinInstance = 0xBC;    // NiGeometry::m_spSkinInstance
	constexpr uintptr_t Data_Vertices = 0x08;       // NiGeometryData::m_usVertices
	constexpr uintptr_t Data_Bound = 0x10;          // NiGeometryData::m_kBound (model space: centre, radius)
	constexpr uintptr_t Data_Vertex = 0x20;         // NiGeometryData::m_pkVertex (model space; freed after upload for some)
	constexpr int VSlot_GetRTTI = 2;

	enum FormType : uint8_t {
		kACTI = 21, kTACT = 22, kTERM = 23, kDOOR = 28, kLIGH = 30, kPWAT = 35, kCharacter = 59, kCreature = 60,
		kMissile = 61, kGrenade = 62, kBeam = 63, kFlame = 64, kContBeam = 105,
	};
	enum LodMult : uint32_t { kLodActors = 3, kLodDistant = 6, kLodBodyPart = 7, kLodMenu = 8, kLodInvisible = 9, kLodImposter = 10 };

	constexpr uintptr_t Call_MainAccumulate = 0x0087415B;   // TESMain::DrawScene -> BSShaderUtil::AccumulateScene 0xB6BEE0
	constexpr uintptr_t Call_MainOpaque = 0x00874180;       // TESMain::DrawScene -> RenderScenePreResolveDepth 0xB6BA20
	constexpr uintptr_t Vtbl_FadeOnVisible = 0x010A9064;    // BSFadeNode vtable slot 53 -> BSFadeNode::OnVisible 0xB4E490
	constexpr uintptr_t Vtbl_TreeOnVisible = 0x010669B8;    // BSTreeNode vtable slot 53 -> BSTreeNode::OnVisible 0x66BF50
	// Actor animation steps (measured in developer builds). The three steps run on the AI threads and in parallel tasks.
	constexpr uintptr_t Fn_ActorUpdateAnimation = 0x00888070;     // Actor::UpdateAnimation (AI thread: sequences, pose, tracking)
	constexpr uintptr_t Fn_ActorUpdateAnimMovement = 0x008885A0;  // Actor::UpdateAnimMovement (parallel task: non-bip controllers, movement)
	constexpr uintptr_t Fn_QueuedScenegraph = 0x00493930;         // Animation::UpdateQueuedScenegraph (after the AI join: bone transforms)
	constexpr uintptr_t Vtbl_ActorUpdateAnimation[] = { 0x010844BC, 0x01086CD4, 0x01087314, 0x0108ACA4 };
	constexpr uintptr_t Vtbl_ActorUpdateAnimMovement[] = { 0x010845A4, 0x01086DBC, 0x010873FC, 0x0108AD8C };
	constexpr uintptr_t Call_QueuedScenegraph[] = {
		0x0096C8ED,   // ProcessLists::UpdateAnimationScenegraph (one thread)
		0x0096CA73,   // ProcessLists::ParallelUpdateAnimationScenegraph (tasklets off)
		0x0096CB40,   // UpdateAnimSceneGraph (its tasklet function)
	};
	constexpr uintptr_t Anim_Actor = 0x04;                        // Animation::pActor
	constexpr uintptr_t Fn_SetDelayUpdateScenegraph = 0x004939B0;   // Animation::SetDelayUpdateScenegraph(bool)
	// Cell update on the AI thread (TES::UpdateCellAnimations 0x453550), measured in developer builds.
	constexpr uintptr_t Fn_CellUpdateManagedNodes = 0x00551890;   // TESObjectCELL::UpdateManagedNodes(time, largeOnly)
	constexpr uintptr_t Call_CellUpdateManagedNodes[] = {
		0x00453608,   // TES::UpdateCellAnimations (interior)
		0x004BAA98,   // GridCellArray::UpdateManagedNodes (each loaded exterior cell)
	};
	// The other calls in TES::UpdateCellAnimations (each checked against its target before it is taken)
	constexpr uintptr_t Call_LockHavok = 0x004535EE;               // TES::LockHavokUpdateMT(true) 0x453860
	constexpr uintptr_t Call_GridUpdate = 0x0045361F;              // GridCellArray::UpdateManagedNodes 0x4BA9A0
	constexpr uintptr_t Call_ParallelTasks = 0x00453648;           // TaskQueueInterface::QueueParallelTaskProcessing 0x87AA90
	constexpr uintptr_t Call_ObjectDamage = 0x00453654;            // FOCollisionListener::DealObjectDamage 0x623640
	constexpr uintptr_t Call_Projectiles = 0x004536CF;             // ProcessLists::PostProcessProjectiles 0x975080
	constexpr uintptr_t Vtbl_bhkWorldUpdate = 0x010C6AB8;          // bhkWorldM vtable slot 49 -> bhkWorld::Update
	constexpr uintptr_t Fn_bhkWorldUpdate = 0x00C6AE70;
	constexpr uintptr_t Vtbl_FadeUpdateSelected = 0x010A9038;      // BSFadeNode vtable slot 42 -> UpdateSelectedDownwardPass
	constexpr uintptr_t Fn_FadeUpdateSelected = 0x00B4EA20;
	// Land: TESObjectCELL::Load3D 0x545CF0 gives each cell's land node four BSMultiBoundNode quadrants (cull mode
	// ALLPASS, one BSMultiBoundAABB each); TESObjectLAND::Attach3D 0x5374F0 puts the quadrant's land strips in child 0
	// and sets the AABB to centre + (1024, 1024, height range / 2).
	constexpr uintptr_t Vtbl_MultiBoundOnVisible = 0x010C1DE8;    // BSMultiBoundNode vtable slot 53
	constexpr uintptr_t Fn_MultiBoundOnVisible = 0x00C46F60;      // BSMultiBoundNode::OnVisible
	constexpr uintptr_t MBNode_MultiBound = 0xAC;                 // BSMultiBoundNode::spMultiBound
	constexpr uintptr_t MB_Shape = 0x0C;                          // BSMultiBound::spShape
	constexpr uintptr_t MBAABB_Center = 0x0C;                     // BSMultiBoundAABB::kCenter
	constexpr uintptr_t MBAABB_HalfExtents = 0x18;                // BSMultiBoundAABB::kHalfExtents
	constexpr uintptr_t Vtbl_MBAABB = 0x0101E8E4;                 // BSMultiBoundAABB (BSMultiBoundOBB 0x101E9A4, Sphere 0x101EA8C)
	constexpr uintptr_t Ni_Name = 0x08;                           // NiObjectNET::m_kName (const char*)
	constexpr uintptr_t Culler_Planes = 0x2C;                     // NiCullingProcess::m_kPlanes (BSMultiBoundNode::OnVisible)
	constexpr int VSlot_MBShapeWithinFrustum = 0x9C / 4;          // BSMultiBoundShape::WithinFrustum(NiFrustumPlanes*)
	// The scene's "Grass" node holds one BSMultiBoundNode per grass group (TallGrassGroup), each with an AABB and one
	// TallGrassTriStrips child.
	constexpr uintptr_t Ni_Parent = 0x18;                         // NiAVObject::m_pkParent
	// TESMain::InitSceneGraph names the scene's roots: "Sky", "Weather", "LODRoot" with "LandLOD" (terrain LOD chunks,
	// BGSTerrainChunk::Attach) and "DistantRefLOD" under it.
	constexpr uintptr_t Call_LampTest = 0x00B5E915;         // ShadowSceneNode::PreOnVisible -> ShadowSceneLight::TestFrustumCull 0xB9E970

	// ShadowSceneLight / NiLight / shapes (IDA local types)
	constexpr uintptr_t SSL_Geometries = 0xE0;     // NiTListBase<NiTriStrips>: head +0, count +8
	constexpr uintptr_t SSL_PointLight = 0xF4;     // bool
	constexpr uintptr_t SSL_Light = 0xF8;          // NiLight*
	constexpr uintptr_t SSL_FrustumCull = 0x110;   // UInt16, 255 = off this frame
	constexpr uintptr_t Light_SpecR = 0xE0;        // NiLight::m_kSpec.r = radius
	constexpr uintptr_t Geo_ShadeProperty = 0xA8;  // NiGeometry m_kProperties (+0x9C) . m_spShadeProperty (+0xC)
	constexpr uintptr_t Shade_Type = 0x1C;         // NiShadeProperty::iShaderPropertyType (lighting properties 1-12)
	constexpr uintptr_t Shade_LightListChanged = 0x74;   // BSShaderLightingProperty::bLightListChanged
	constexpr uintptr_t Culler_Camera = 0x0C;      // NiCullingProcess::m_pkCamera

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
	// Objects only:
	float corner[8][3] = {};                   // the tested box's corners (world); mn/mx then hold it too
	bool oriented = false;                     // the box is rot * [lmn, lmx] + org
	float rot[9] = {}, org[3] = {}, lmn[3] = {}, lmx[3] = {};
	bool boxPartial = false;                   // built with the frame's vertex budget spent: built again later
	uint32_t boxGen = 0;                       // box mode it was built in (the compare key changes it)
	bool actor = false;                        // an NPC or creature: box rebuilt every frame
	bool door = false;                         // a door, activator or terminal: box rebuilt every frame
	bool issuedGrown = false;                  // its pending query's box was grown for camera movement
	bool afterGrown = false;                   // a grown box showed pixels: tested next frame, one 0 restores the streak
	uint8_t streakBeforeGrown = 0;             // the 0-pixel streak it had before that
	bool tree = false;                         // a SpeedTree (BSTreeNode): box = bound cube + leaf margin
	bool land = false;                         // a land quadrant (BSMultiBoundNode): box = its multibound AABB
	float tmn[3] = {}, tmx[3] = {};            // the box its pending query was drawn with
	float amn[3] = {}, amx[3] = {};            // the box its newest answer was about
};

struct Candidate {
	Record* rec;
	bool wouldHide;
};

static std::unordered_map<void*, Record> gRecords;

struct Lamp {
	Record rec;                    // its reach box (rec.mn/mx) and query answers
	void* light = nullptr;         // NiLight (identity)
	uint32_t seenFrame = 0;        // last frame the main view tested it
	bool decision = false;         // hidden this frame (decided once per frame)
	bool overrideActive = false;   // we wrote 255 and must put savedCull back before the next test
	uint16_t savedCull = 0;
	bool hiddenApplied = false;    // our switch-off as of the last main-view frame (shapes marked when it changes)
	bool candidate = false;        // tested this frame
	bool nearCamera = false;
};
static std::unordered_map<void*, Lamp> gLamps;
static std::vector<Lamp*> gLampCandidates;
static std::vector<Candidate> gCandidates;
static std::vector<IDirect3DQuery9*> gFreeQueries;
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
static bool gOrientedOn = true;     // bOrientedBoxes, then the compare key
static bool gActorsOn = true;       // bActorCulling, then the compare key
static bool gTreesOn = true;        // bTreeCulling, then the compare key
static bool gActivatorsOn = true;   // bActivatorCulling, then the compare key
static bool gRevealOn = true;       // fRevealDepth > 0, then the compare key
static bool gLandOn = true;         // bLandCulling, then the compare key
static bool gGrassOn = true;        // bGrassCulling, then the compare key
static bool gAnimThrottleOn = true; // bAnimThrottle, then the compare key
constexpr float kRevealMinMove = 0.5f;   // camera moves below this (units per frame) grow nothing
// 0 the INI's settings, 1 land flipped, 2 grass flipped, 3 actor bone throttle flipped (the features still being compared)
static int gCompareMode = 0;
static uint32_t gBoxGen = 1;        // changes when the box mode does: boxes are built again
static bool gCompareMessage = false;
static int gVertexBudget = 0;       // vertices still to be measured this frame
constexpr int kVertexBudget = 150000;
// Camera per frame (ring), for the answer guard
constexpr int kRing = 16;
static float gCamPos[kRing][3], gCamRot[kRing][9];
static float gNowPos[3], gNowRot[9];
static bool gGuardOk[kRing];        // per answer age: has the main camera stayed within the guard since? (once per frame)

// Queries waiting for their answers, in the order they were issued (objects and lamps). Records stay put while listed:
// unordered_map nodes never move, and pending records are never erased.
static std::vector<Record*> gPending;

// ---------------------------------------------------------------------------------------------------------------
// Statistics (sums over the log block)

struct Stats {
	double frames = 0;
	double topFade = 0, candidates = 0, noRef = 0, actors = 0, kinds = 0, lod = 0, smallObj = 0, nearSkip = 0, culledFlag = 0;
	double wouldHide = 0, hideGeoms = 0, candGeoms = 0, guardBlocked = 0, tooOld = 0, noAnswer = 0;
	double issued = 0, answered = 0, notReady = 0, failed = 0, budgetSkips = 0, ageSum = 0, zeroAnswers = 0;
	double issuedCovered = 0, issuedNew = 0, issuedVisible = 0, notDue = 0, behindCamera = 0, alphaTestFrames = 0;
	double alphaTestedFrames = 0;   // frames tested with alpha testing on (full-alpha boxes)
	double clippedBoxes = 0;        // boxes reaching behind the camera or far off screen, tested cut to what can be seen
	double camMismatch = 0, noDepth = 0, identityResets = 0;
	double hidden = 0, hiddenGeoms = 0;                    // actually not drawn (hiding on)
	double lampsOn = 0, lampsTested = 0, lampsNear = 0, lampsCovered = 0, lampShapes = 0, lampShapesCovered = 0;
	double lampsOff = 0, lampFlags = 0, lampsWaiting = 0;
	double onMs = 0, onFrames = 0, offMs = 0, offFrames = 0;   // frame time while hiding is on / off
	double queryUs = 0, pollUs = 0, planUs = 0, prepUs = 0, drawUs = 0, pollsSkipped = 0, sorted = 0;
	double revealFrames = 0, revealPx = 0, revealMaxPx = 0, revealGrown = 0, revealShown = 0, revealRestored = 0;
	double records = 0;
	double boxesBuilt = 0, boxesOriented = 0, boxesAxisTighter = 0, boxesAnimated = 0, boxesPartial = 0, volumeRatio = 0;
	double vertexShapes = 0, sphereShapes = 0, oddShapes = 0;
	struct Moving {
		double seen = 0, cands = 0, nearCam = 0, covered = 0, hidden = 0, moved = 0, guard = 0, old = 0, geoms = 0, hiddenGeoms = 0;
		double noShapes = 0;
	} actor, door;
	double treeSeen = 0, treeCands = 0, treeHidden = 0, treeGeoms = 0, treeHiddenGeoms = 0;
	double landSeen = 0, landQuads = 0, landNear = 0, landCands = 0, landCovered = 0, landHidden = 0, landGuard = 0;
	double lodSeen = 0, lodNear = 0, lodCands = 0, lodCovered = 0, lodHidden = 0, lodGuard = 0;
	double grassSeen = 0, grassNear = 0, grassCands = 0, grassCovered = 0, grassHidden = 0, grassGuard = 0;
	double mbOutside = 0;   // land / LOD / grass nodes outside the view (left to the game's own test)
	double lodImposter = 0, lodMenu = 0, lodInvisible = 0;   // not candidates, by kind
	double impSeen = 0, impCands = 0, impGeoms = 0, impHidden = 0, impHiddenGeoms = 0;
} gStats;
static LARGE_INTEGER gQpcFreq;

// Actors the main view does not draw: per actor ref, the newest main-view frame that reached it and the newest one that
// hid it. The animation steps (AI threads, parallel tasks) read it, so it sits behind a lock. Developer builds also time
// the engine's three per-actor steps, split into drawn / hidden by us / outside the main view.
namespace AnimCost {
	enum Step { kUpdate, kPose, kBones, kSteps };
	enum Class { kDrawn, kHidden, kOutside, kClasses };
	struct Sum { volatile LONG64 ticks[kClasses] = {}, calls[kClasses] = {}; };
	static Sum gSum[kSteps];
	static volatile LONG64 gSkipped[kClasses];                     // bone updates left for a later frame
	static SRWLOCK gLock = SRWLOCK_INIT;
	struct Seen { uint32_t seen = 0, hidden = 0; };
	static std::unordered_map<void*, Seen> gActors;
	static bool gHooked[kSteps];

	static void Mark(void* ref, uint32_t frame, bool hidden) {
		AcquireSRWLockExclusive(&gLock);
		Seen& s = gActors[ref];
		s.seen = frame;
		if (hidden) s.hidden = frame;
		ReleaseSRWLockExclusive(&gLock);
	}
	// Hidden: hidden by us in the newest main view (or the one being drawn). Outside: no main view reached it in the last
	// two frames. Anything in between counts as drawn.
	static Class Classify(void* ref, uint32_t now) {
		AcquireSRWLockShared(&gLock);
		auto it = gActors.find(ref);
		Class c = kOutside;
		if (it != gActors.end()) {
			if (it->second.hidden + 1 >= now) c = kHidden;
			else if (it->second.seen + 2 >= now) c = kDrawn;
		}
		ReleaseSRWLockShared(&gLock);
		return c;
	}
	static void Prune(uint32_t now) {
		AcquireSRWLockExclusive(&gLock);
		for (auto it = gActors.begin(); it != gActors.end();)
			it = it->second.seen + 600 < now ? gActors.erase(it) : std::next(it);
		ReleaseSRWLockExclusive(&gLock);
	}
}

namespace CellCost {
	static volatile LONG64 gPhysTicks, gPhysCalls, gPhysInCellTicks, gCellTicks, gCellCalls;
	static volatile LONG64 gObjTicks[AnimCost::kClasses], gObjCalls[AnimCost::kClasses];
	static SRWLOCK gLock = SRWLOCK_INIT;
	static std::unordered_map<void*, AnimCost::Seen> gNodes;      // BSFadeNode -> main-view frames
	static thread_local int tInCell = 0, tInObject = 0;
	enum Part { kLock, kGrid, kTasks, kDamage, kProjectiles, kParts };
	static volatile LONG64 gPartTicks[kParts], gPartCalls[kParts];
	static uintptr_t gPartOrig[kParts];
	static bool gHooked = false;

	static void Mark(void* node, uint32_t frame, bool hidden) {
		AcquireSRWLockExclusive(&gLock);
		AnimCost::Seen& s = gNodes[node];
		s.seen = frame;
		if (hidden) s.hidden = frame;
		ReleaseSRWLockExclusive(&gLock);
	}
	static AnimCost::Class Classify(void* node, uint32_t now) {
		AcquireSRWLockShared(&gLock);
		auto it = gNodes.find(node);
		AnimCost::Class c = AnimCost::kOutside;
		if (it != gNodes.end()) {
			if (it->second.hidden + 1 >= now) c = AnimCost::kHidden;
			else if (it->second.seen + 2 >= now) c = AnimCost::kDrawn;
		}
		ReleaseSRWLockShared(&gLock);
		return c;
	}
	static void Prune(uint32_t now) {
		AcquireSRWLockExclusive(&gLock);
		for (auto it = gNodes.begin(); it != gNodes.end();)
			it = it->second.seen + 600 < now ? gNodes.erase(it) : std::next(it);
		ReleaseSRWLockExclusive(&gLock);
	}
	static LONG64 Ticks(const LARGE_INTEGER& a) {
		LARGE_INTEGER b;
		QueryPerformanceCounter(&b);
		return b.QuadPart - a.QuadPart;
	}
}
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

struct Shape {
	float mn[3], mx[3]; int geoms, visited; bool any;   // union of the shapes' world bound cubes
	// Tighter boxes (rot set): in the root's frame (omn/omx) and along the world axes (wmn/wmx)
	const float* rot; const float* org;
	float omn[3], omx[3], wmn[3], wmx[3]; bool tightAny;
	int vertexShapes, sphereShapes, oddShapes; bool animated, partial;
};

// A shape's extent in its own (model) space, from its vertices: measured once per mesh data and cached.
struct ModelBox { const float* verts; uint16_t count; float bound[4]; float mn[3], mx[3]; };
static std::unordered_map<void*, ModelBox> gModelBoxes;

static bool ShapeModelBox(void* geo, float* mn, float* mx, Shape& s) {
	if (!IsKindOf(geo, Game::RTTI_NiTriBasedGeom) || Game::At<void*>(geo, Game::Geo_SkinInstance)) return false;
	void* data = Game::At<void*>(geo, Game::Geo_ModelData);
	if (!data) return false;
	const uint16_t n = Game::At<uint16_t>(data, Game::Data_Vertices);
	const float* v = Game::At<const float*>(data, Game::Data_Vertex);
	if (!v || !n) return false;
	const float* bound = &Game::At<float>(data, Game::Data_Bound);
	auto it = gModelBoxes.find(data);
	if (it != gModelBoxes.end() && it->second.verts == v && it->second.count == n && !memcmp(it->second.bound, bound, 16)) {
		for (int i = 0; i < 3; ++i) { mn[i] = it->second.mn[i]; mx[i] = it->second.mx[i]; }
		return true;
	}
	if (gVertexBudget < n) { s.partial = true; return false; }
	gVertexBudget -= n;
	for (int i = 0; i < 3; ++i) mn[i] = mx[i] = v[i];
	for (uint32_t k = 1; k < n; ++k)
		for (int i = 0; i < 3; ++i) {
			const float x = v[k * 3 + i];
			if (x < mn[i]) mn[i] = x;
			if (x > mx[i]) mx[i] = x;
		}
	// The vertices must lie in the data's own bound sphere (the engine computes it from them): anything else is not
	// what we think it is, and the shape keeps its bound sphere.
	const float slack = bound[3] * 0.01f + 1.0f;
	for (int i = 0; i < 3; ++i)
		if (!(mn[i] >= bound[i] - bound[3] - slack) || !(mx[i] <= bound[i] + bound[3] + slack)) { ++s.oddShapes; return false; }
	if (gModelBoxes.size() > 50000) gModelBoxes.clear();
	ModelBox& e = gModelBoxes[data];
	e.verts = v; e.count = n;
	memcpy(e.bound, bound, 16);
	for (int i = 0; i < 3; ++i) { e.mn[i] = mn[i]; e.mx[i] = mx[i]; }
	return true;
}

static void Grow(bool& any, float* mn, float* mx, const float* bmn, const float* bmx) {
	for (int i = 0; i < 3; ++i) {
		mn[i] = any ? min(mn[i], bmn[i]) : bmn[i];
		mx[i] = any ? max(mx[i], bmx[i]) : bmx[i];
	}
	any = true;
}

static void WalkShapes(void* object, int depth, Shape& s) {
	if (!object || depth > 32 || ++s.visited > 4096) return;
	if (Game::At<uint32_t>(object, Game::Ni_Flags) & 1u) return;   // APP_CULLED: not drawn
	if (Game::At<void*>(object, Game::Obj_Controllers)) s.animated = true;
	if (IsKindOf(object, Game::RTTI_NiNode)) {
		void** children = Game::At<void**>(object, Game::Node_ChildBase);
		const uint16_t count = Game::At<uint16_t>(object, Game::Node_ChildSize);
		for (uint16_t i = 0; children && i < count; ++i) WalkShapes(children[i], depth + 1, s);
	} else if (IsKindOf(object, Game::RTTI_NiGeometry)) {
		++s.geoms;
		const float* b = Game::At<const float*>(object, Game::Ni_WorldBound);
		if (!b || !(b[3] > 0.0f)) return;
		const float smn[3] = { b[0] - b[3], b[1] - b[3], b[2] - b[3] }, smx[3] = { b[0] + b[3], b[1] + b[3], b[2] + b[3] };
		Grow(s.any, s.mn, s.mx, smn, smx);
		if (!s.rot) return;
		float amn[3], amx[3], wmn[3], wmx[3], lmn[3], lmx[3];
		if (ShapeModelBox(object, amn, amx, s)) {
			// world = R_g * (s_g * x) + t_g; in the root's frame: R^T * (world - org)
			const float* gr = &Game::At<float>(object, Game::Ni_WorldRotate);
			const float* gt = &Game::At<float>(object, Game::Ni_WorldTranslate);
			const float gs = Game::At<float>(object, Game::Ni_WorldScale);
			float mw[9], ml[9], ol[3];
			for (int k = 0; k < 9; ++k) mw[k] = gr[k] * gs;
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
					ml[i * 3 + j] = s.rot[0 * 3 + i] * mw[0 * 3 + j] + s.rot[1 * 3 + i] * mw[1 * 3 + j] + s.rot[2 * 3 + i] * mw[2 * 3 + j];
			OcclusionMath::ToLocal(s.rot, s.org, gt, ol);
			OcclusionMath::TransformedBox(mw, gt, amn, amx, wmn, wmx);
			OcclusionMath::TransformedBox(ml, ol, amn, amx, lmn, lmx);
			++s.vertexShapes;
		} else {
			float c[3];
			OcclusionMath::ToLocal(s.rot, s.org, b, c);
			for (int i = 0; i < 3; ++i) {
				wmn[i] = smn[i]; wmx[i] = smx[i];
				lmn[i] = c[i] - b[3]; lmx[i] = c[i] + b[3];
			}
			++s.sphereShapes;
		}
		bool any = s.tightAny;
		Grow(any, s.wmn, s.wmx, wmn, wmx);
		Grow(s.tightAny, s.omn, s.omx, lmn, lmx);
	}
}

// The old box is the union of the shapes' bound cubes, cut to the node's own bound cube (both contain every shape).
// With oriented boxes on, the shapes' measured extents give two more boxes, in the object's frame and along the world
// axes (each cut to the node's bound too); the smallest box holding the whole object is tested.
static void RebuildShape(void* node, const float* bound, Record& rec) {
	Shape s{};
	if (rec.tree) {   // the bound's cube plus room for the leaf cards (see the header)
		WalkShapes(node, 0, s);
		rec.geoms = (uint16_t)min(s.geoms, 65535);
		const float reach = bound[3] * 1.2f + gSet.treeMargin;
		for (int i = 0; i < 3; ++i) { rec.mn[i] = bound[i] - reach; rec.mx[i] = bound[i] + reach; }
		rec.oriented = false;
		rec.boxPartial = false;
		rec.boxGen = gBoxGen;
		OcclusionMath::AxisCorners(rec.mn, rec.mx, rec.corner);
		return;
	}
	const float* rot = &Game::At<float>(node, Game::Ni_WorldRotate);
	const float* org = &Game::At<float>(node, Game::Ni_WorldTranslate);
	// The local-frame maths needs a true rotation (rot^T * rot = identity); anything else keeps the old box.
	bool rotation = true;
	for (int i = 0; i < 3 && rotation; ++i)
		for (int j = 0; j < 3; ++j) {
			const float d = rot[0 * 3 + i] * rot[0 * 3 + j] + rot[1 * 3 + i] * rot[1 * 3 + j] + rot[2 * 3 + i] * rot[2 * 3 + j];
			if (!(std::fabs(d - (i == j ? 1.0f : 0.0f)) < 0.001f)) { rotation = false; break; }
		}
	if (gOrientedOn && rotation) { s.rot = rot; s.org = org; }
	WalkShapes(node, 0, s);
	rec.geoms = (uint16_t)min(s.geoms, 65535);
	for (int i = 0; i < 3; ++i) {
		const float lo = bound[i] - bound[3], hi = bound[i] + bound[3];
		rec.mn[i] = s.any ? max(s.mn[i], lo) : lo;
		rec.mx[i] = s.any ? min(s.mx[i], hi) : hi;
		if (rec.mx[i] < rec.mn[i]) { rec.mn[i] = lo; rec.mx[i] = hi; }
	}
	rec.oriented = false;
	rec.boxPartial = s.partial;
	rec.boxGen = gBoxGen;
	gStats.boxesBuilt += 1;
	if (s.partial) gStats.boxesPartial += 1;
	const float oldVolume = OcclusionMath::BoxVolume(rec.mn, rec.mx);
	float volume = oldVolume;
	if (gOrientedOn && s.animated) gStats.boxesAnimated += 1;
	if (gOrientedOn && s.tightAny && !s.animated) {
		gStats.vertexShapes += s.vertexShapes;
		gStats.sphereShapes += s.sphereShapes;
		gStats.oddShapes += s.oddShapes;
		float wmn[3], wmx[3], lmn[3], lmx[3], c[3];
		bool axisOk = true, localOk = true;
		OcclusionMath::ToLocal(rot, org, bound, c);
		for (int i = 0; i < 3; ++i) {
			wmn[i] = max(s.wmn[i], rec.mn[i]); wmx[i] = min(s.wmx[i], rec.mx[i]);
			lmn[i] = max(s.omn[i], c[i] - bound[3]); lmx[i] = min(s.omx[i], c[i] + bound[3]);
			axisOk = axisOk && wmn[i] <= wmx[i];
			localOk = localOk && lmn[i] <= lmx[i];
		}
		if (axisOk && OcclusionMath::BoxVolume(wmn, wmx) < volume) {
			for (int i = 0; i < 3; ++i) { rec.mn[i] = wmn[i]; rec.mx[i] = wmx[i]; }
			volume = OcclusionMath::BoxVolume(wmn, wmx);
			gStats.boxesAxisTighter += 1;
		}
		if (localOk && OcclusionMath::BoxVolume(lmn, lmx) < volume * 0.98f) {
			rec.oriented = true;
			for (int i = 0; i < 9; ++i) rec.rot[i] = rot[i];
			for (int i = 0; i < 3; ++i) { rec.org[i] = org[i]; rec.lmn[i] = lmn[i]; rec.lmx[i] = lmx[i]; }
			volume = OcclusionMath::BoxVolume(lmn, lmx);
			gStats.boxesOriented += 1;
		}
	}
	if (oldVolume > 0.0f) gStats.volumeRatio += volume / oldVolume;
	else gStats.volumeRatio += 1.0;
	if (rec.oriented) {
		OcclusionMath::OrientedCorners(rec.rot, rec.org, rec.lmn, rec.lmx, rec.corner);
		// mn/mx (groups' unions) = the oriented box's world extent, cut to the axis box: both hold the object
		float cmn[3], cmx[3];
		for (int i = 0; i < 3; ++i) { cmn[i] = cmx[i] = rec.corner[0][i]; }
		for (int k = 1; k < 8; ++k)
			for (int i = 0; i < 3; ++i) { cmn[i] = min(cmn[i], rec.corner[k][i]); cmx[i] = max(cmx[i], rec.corner[k][i]); }
		for (int i = 0; i < 3; ++i) { rec.mn[i] = max(rec.mn[i], cmn[i]); rec.mx[i] = min(rec.mx[i], cmx[i]); }
	} else {
		OcclusionMath::AxisCorners(rec.mn, rec.mx, rec.corner);
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Collecting (main pass, inside BSFadeNode::OnVisible)

// May the newest answer be used now? Its age in frames, and the camera's turn and move since it was drawn.
static bool AnswerUsableAt(const Record& rec, bool& tooOld, const float* nowPos, const float* nowRot);

// The guard for every answer age at once, for the main camera (an acos per age per frame, not per object).
static void ComputeGuard() {
	for (uint32_t age = 1; age < (uint32_t)kRing; ++age) {
		const int slot = (gFrame - age) % kRing;
		const float* p = gCamPos[slot];
		const float dx = gNowPos[0] - p[0], dy = gNowPos[1] - p[1], dz = gNowPos[2] - p[2];
		gGuardOk[age] = std::sqrt(dx * dx + dy * dy + dz * dz) <= gSet.guardUnits * age &&
			OcclusionMath::RotationAngleDegrees(gNowRot, gCamRot[slot]) <= gSet.guardDegrees * age;
	}
}

// AnswerUsableAt for the main camera, from the table.
static bool AnswerUsable(const Record& rec, bool& tooOld) {
	tooOld = false;
	const uint32_t age = gFrame - rec.answerFrame;
	if (age < 1 || age > (uint32_t)gSet.maxAnswerAge || age >= kRing) { tooOld = true; return false; }
	return gGuardOk[age];
}

static bool AnswerUsableAt(const Record& rec, bool& tooOld, const float* nowPos, const float* nowRot) {
	tooOld = false;
	const uint32_t age = gFrame - rec.answerFrame;
	if (age < 1 || age > (uint32_t)gSet.maxAnswerAge || age >= kRing) { tooOld = true; return false; }
	const int slot = rec.answerFrame % kRing;
	const float* p = gCamPos[slot];
	const float dx = nowPos[0] - p[0], dy = nowPos[1] - p[1], dz = nowPos[2] - p[2];
	if (std::sqrt(dx * dx + dy * dy + dz * dz) > gSet.guardUnits * age) return false;
	return OcclusionMath::RotationAngleDegrees(nowRot, gCamRot[slot]) <= gSet.guardDegrees * age;
}

// Things that move (NPCs and creatures; doors, activators and terminals): box = world bound sphere + margin, every
// frame; hidden while the current bound is still inside the box of the newest 0-pixel answer (and the camera guard
// holds, as for objects).
static bool CollectMoving(void* node, void* ref, const float* bound, bool isActor) {
	Stats::Moving& st = isActor ? gStats.actor : gStats.door;
	Record& rec = gRecords[node];
	const uint32_t formID = Game::At<uint32_t>(ref, Game::Form_ID);
	if (rec.lastSeen == 0 || rec.formID != formID || rec.actor != isActor || rec.door == isActor || rec.land) {
		if (rec.lastSeen != 0) gStats.identityResets += 1;
		if (rec.pending) { rec.query->Release(); rec.query = nullptr; }
		rec.formID = formID;
		rec.pending = false; rec.answered = false; rec.zeroStreak = 0; rec.afterGrown = false; rec.pixels = 0;
		rec.oriented = false; rec.boxPartial = false; rec.tree = false; rec.land = false;
		rec.actor = isActor;
		rec.door = !isActor;
		Shape s{};
		WalkShapes(node, 0, s);   // shape count (nothing to draw: never tested)
		rec.geoms = (uint16_t)min(s.geoms, 65535);
	}
	rec.lastSeen = gFrame;
	rec.boxGen = gBoxGen;
	st.seen += 1;
	if (!rec.geoms) { st.noShapes += 1; return false; }
	const float r = bound[3];
	const float margin = isActor ? r * 0.1f + gSet.actorMargin : r * 0.05f + gSet.activatorMargin;
	float cmn[3], cmx[3];
	for (int i = 0; i < 3; ++i) {
		cmn[i] = bound[i] - r; cmx[i] = bound[i] + r;
		rec.mn[i] = cmn[i] - margin; rec.mx[i] = cmx[i] + margin;
	}
	OcclusionMath::AxisCorners(rec.mn, rec.mx, rec.corner);
	if (OcclusionMath::DistanceToBox(gNowPos, rec.mn, rec.mx) < gSet.nearDistance) { st.nearCam += 1; return false; }

	st.cands += 1;
	st.geoms += rec.geoms;
	bool hide = false;
	if (rec.answered && rec.zeroStreak >= 2) {
		st.covered += 1;
		bool inside = true;
		for (int i = 0; i < 3; ++i) inside = inside && cmn[i] >= rec.amn[i] && cmx[i] <= rec.amx[i];
		bool tooOld;
		if (!inside) st.moved += 1;
		else if (AnswerUsable(rec, tooOld)) hide = true;
		else if (tooOld) st.old += 1;
		else st.guard += 1;
	}
	gCandidates.push_back({ &rec, hide });
	if (hide && gCullingOn) { st.hidden += 1; st.hiddenGeoms += rec.geoms; }
	if (isActor && hide && gCullingOn) AnimCost::Mark(ref, gFrame, true);
	if (kDevTools && hide && gCullingOn) CellCost::Mark(node, gFrame, true);
	return hide;
}

static __declspec(noinline) bool Collect(void* node, bool tree) {
	if (tree) {
		gStats.treeSeen += 1;
		if (!gTreesOn) return false;
	}
	gStats.topFade += 1;
	if (kDevTools) CellCost::Mark(node, gFrame, false);
	void* ref = Game::At<void*>(node, Game::Fade_LinkedRef);
	if (!ref) { gStats.noRef += 1; return false; }
	const uint8_t refType = Game::At<uint8_t>(ref, Game::Form_Type);
	void* base = Game::At<void*>(ref, Game::Ref_Base);
	const uint8_t baseType = base ? Game::At<uint8_t>(base, Game::Form_Type) : 0;
	const uint32_t mult = Game::At<uint32_t>(node, Game::Fade_MultType);
	const float* bound = Game::At<const float*>(node, Game::Ni_WorldBound);

	bool skip = true;
	const bool isActorRef = (refType == Game::kCharacter || refType == Game::kCreature) && ref != *(void**)Game::PlayerPtr;
	if (isActorRef) AnimCost::Mark(ref, gFrame, false);   // reached by the main view (CollectMoving marks the hidden ones)
	if (gActorsOn && (refType == Game::kCharacter || refType == Game::kCreature) && mult != Game::kLodBodyPart &&
		ref != *(void**)Game::PlayerPtr && bound && bound[3] > 0.0f && bound[3] < 10000.0f &&
		!(Game::At<uint32_t>(node, Game::Ni_Flags) & 1u)) {
		return CollectMoving(node, ref, bound, true);
	}
	if (gActivatorsOn && (baseType == Game::kDOOR || baseType == Game::kACTI || baseType == Game::kTACT ||
		baseType == Game::kTERM) && refType != Game::kCharacter && refType != Game::kCreature && bound &&
		bound[3] >= gSet.minRadius && bound[3] < 100000.0f && !(Game::At<uint32_t>(node, Game::Ni_Flags) & 1u)) {
		return CollectMoving(node, ref, bound, false);
	}
	if (refType == Game::kCharacter || refType == Game::kCreature || mult == Game::kLodActors || mult == Game::kLodBodyPart)
		gStats.actors += 1;
	else if ((refType >= Game::kMissile && refType <= Game::kFlame) || refType == Game::kContBeam || baseType == Game::kDOOR ||
	         baseType == Game::kLIGH || baseType == Game::kACTI || baseType == Game::kTACT || baseType == Game::kTERM ||
	         baseType == Game::kPWAT)
		gStats.kinds += 1;
	else if (mult == Game::kLodDistant && !gSet.distantLOD)
		gStats.lod += 1;
	else if (mult == Game::kLodImposter && !gSet.imposters)
		gStats.lodImposter += 1;
	else if (mult == Game::kLodMenu)
		gStats.lodMenu += 1;
	else if (mult == Game::kLodInvisible)
		gStats.lodInvisible += 1;
	else if (!bound || !(bound[3] >= gSet.minRadius))
		gStats.smallObj += 1;
	else if (Game::At<uint32_t>(node, Game::Ni_Flags) & 1u)
		gStats.culledFlag += 1;
	else
		skip = false;
	if (mult == Game::kLodImposter) gStats.impSeen += 1;
	if (skip) return false;

	Record& rec = gRecords[node];
	const uint32_t formID = Game::At<uint32_t>(ref, Game::Form_ID);
	const float* pos = &Game::At<float>(node, Game::Ni_WorldTranslate);
	const float scale = Game::At<float>(node, Game::Ni_WorldScale);
	const bool same = rec.lastSeen != 0 && !rec.actor && !rec.door && !rec.land && rec.tree == tree && rec.formID == formID && rec.scale == scale &&
		rec.pos[0] == pos[0] && rec.pos[1] == pos[1] && rec.pos[2] == pos[2];
	if (!same) {
		if (rec.lastSeen != 0) gStats.identityResets += 1;
		if (rec.pending) { rec.query->Release(); rec.query = nullptr; }   // its answer belongs to the old object
		rec.formID = formID; rec.scale = scale;
		rec.pos[0] = pos[0]; rec.pos[1] = pos[1]; rec.pos[2] = pos[2];
		rec.pending = false; rec.answered = false; rec.zeroStreak = 0; rec.afterGrown = false; rec.pixels = 0;
		rec.actor = false;
		rec.door = false;
		rec.land = false;
		rec.tree = tree;
		RebuildShape(node, bound, rec);
	} else if (rec.boxGen != gBoxGen || rec.boxPartial) {
		RebuildShape(node, bound, rec);   // another box mode, or measured in full now: both boxes hold the object, answers stay
	}
	rec.lastSeen = gFrame;

	const float distance = rec.oriented ? OcclusionMath::DistanceToOrientedBox(gNowPos, rec.rot, rec.org, rec.lmn, rec.lmx) :
		OcclusionMath::DistanceToBox(gNowPos, rec.mn, rec.mx);
	if (distance < gSet.nearDistance) { gStats.nearSkip += 1; return false; }

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
	if (kDevTools && hide && gCullingOn) CellCost::Mark(node, gFrame, true);
	if (mult == Game::kLodImposter) {
		gStats.impCands += 1;
		gStats.impGeoms += rec.geoms;
		if (hide && gCullingOn) { gStats.impHidden += 1; gStats.impHiddenGeoms += rec.geoms; }
	}
	if (tree) {
		gStats.treeCands += 1;
		gStats.treeGeoms += rec.geoms;
		if (hide && gCullingOn) { gStats.treeHidden += 1; gStats.treeHiddenGeoms += rec.geoms; }
	}
	return hide;
}

static void OnFault(const char* where) {
	gFaulted = true;
	Log("Error: an exception in %s; the plugin has switched itself off for this session", where);
}

// True when the object is to be hidden. An exception draws it (and switches the plugin off).
// Multibound nodes (BSMultiBoundNode::OnVisible). Land quadrants: the box is the quadrant's own multibound AABB (the
// land's real height range), plus fLandMargin up and down. Distant terrain LOD chunks (any node below "LandLOD"): the
// chunk's AABB, same margin. Static boxes, kept while the AABB stays where it was; hidden on the same rule as objects.
// Everything else is left alone (developer builds tally it by kind for the log).

static const char* NodeName(void* node) { return node ? Game::At<const char*>(node, Game::Ni_Name) : nullptr; }
static bool NameIs(void* node, const char* name) { const char* n = NodeName(node); return n && strcmp(n, name) == 0; }
static const char* RttiName(void* object) {
	const void* r = (*(GetRTTI_t**)object)[Game::VSlot_GetRTTI](object);
	return r ? *(const char* const*)r : "?";
}

static std::unordered_map<std::string, double> gOtherMultiBounds;   // kind -> count (sampled every 8th frame)

static void TallyMultiBound(void* node, void* shape) {
	void* parent = Game::At<void*>(node, Game::Ni_Parent);
	void* grand = parent ? Game::At<void*>(parent, Game::Ni_Parent) : nullptr;
	const uint16_t n = Game::At<uint16_t>(node, Game::Node_ChildSize);
	void* child = n ? *Game::At<void**>(node, Game::Node_ChildBase) : nullptr;
	char key[200];
	const char* own = NodeName(node);
	const char* pn = NodeName(parent);
	const char* gn = NodeName(grand);
	const uintptr_t sv = shape ? *(uintptr_t*)shape : 0;
	const char* st = sv == Game::Vtbl_MBAABB ? "AABB" : sv == 0x0101E9A4 ? "OBB" : sv == 0x0101EA8C ? "sphere" : shape ? "other" : "none";
	char size[32] = "";
	if (sv == Game::Vtbl_MBAABB) {
		const float* h = &Game::At<float>(shape, Game::MBAABB_HalfExtents);
		snprintf(size, sizeof size, " %.0fx%.0fx%.0f", 2 * h[0], 2 * h[1], 2 * h[2]);
	}
	snprintf(key, sizeof key, "'%s' in '%s' in '%s', %s%s, %u children, first %s", own ? own : "", pn ? pn : "", gn ? gn : "",
		st, size, (unsigned)n, child ? RttiName(child) : "-");
	gOtherMultiBounds[key] += 8.0;
}

enum BoxedKind { kBoxLand, kBoxLod, kBoxGrass };

static bool CollectBoxed(void* node, const float* c, const float* h, BoxedKind kind) {
	double* st[6];   // near, candidates, covered, hidden, guard (by kind)
	if (kind == kBoxLand) { st[0] = &gStats.landNear; st[1] = &gStats.landCands; st[2] = &gStats.landCovered; st[3] = &gStats.landHidden; st[4] = &gStats.landGuard; }
	else if (kind == kBoxLod) { st[0] = &gStats.lodNear; st[1] = &gStats.lodCands; st[2] = &gStats.lodCovered; st[3] = &gStats.lodHidden; st[4] = &gStats.lodGuard; }
	else { st[0] = &gStats.grassNear; st[1] = &gStats.grassCands; st[2] = &gStats.grassCovered; st[3] = &gStats.grassHidden; st[4] = &gStats.grassGuard; }
	Record& rec = gRecords[node];
	if (rec.lastSeen == 0 || !rec.land || rec.pos[0] != c[0] || rec.pos[1] != c[1] || rec.pos[2] != c[2] || rec.scale != h[2] ||
		rec.lmn[0] != h[0]) {
		if (rec.lastSeen != 0) gStats.identityResets += 1;
		if (rec.pending) { rec.query->Release(); rec.query = nullptr; }
		rec.formID = 0;
		rec.pos[0] = c[0]; rec.pos[1] = c[1]; rec.pos[2] = c[2];
		rec.scale = h[2];
		rec.lmn[0] = h[0];   // unused for these boxes (not oriented): part of the identity
		rec.pending = false; rec.answered = false; rec.zeroStreak = 0; rec.afterGrown = false; rec.pixels = 0;
		rec.oriented = false; rec.boxPartial = false; rec.tree = false; rec.actor = false; rec.door = false;
		rec.land = true;
		rec.geoms = 1;
		for (int i = 0; i < 3; ++i) {
			const float m = kind == kBoxGrass ? gSet.grassMargin : (i == 2 ? gSet.landMargin : 0.0f);
			rec.mn[i] = c[i] - h[i] - m;
			rec.mx[i] = c[i] + h[i] + m;
		}
		OcclusionMath::AxisCorners(rec.mn, rec.mx, rec.corner);
	}
	rec.lastSeen = gFrame;
	rec.boxGen = gBoxGen;
	if (OcclusionMath::DistanceToBox(gNowPos, rec.mn, rec.mx) < gSet.nearDistance) { *st[0] += 1; return false; }
	*st[1] += 1;
	bool hide = false;
	if (rec.answered && rec.zeroStreak >= 2) {
		*st[2] += 1;
		bool tooOld;
		hide = AnswerUsable(rec, tooOld);
		if (!hide) *st[4] += 1;
	}
	gCandidates.push_back({ &rec, hide });
	if (hide && gCullingOn) *st[3] += 1;
	return hide;
}

typedef bool(__thiscall* WithinFrustum_t)(void*, void*);

// The game's own test for this node's box against the main view's planes (what BSMultiBoundNode::OnVisible does).
static bool ShapeInView(void* shape, void* culler) {
	return (*(WithinFrustum_t**)shape)[Game::VSlot_MBShapeWithinFrustum](shape, (char*)culler + Game::Culler_Planes);
}

static bool CollectLand(void* node, void* culler) {
	gStats.landSeen += 1;
	void* mb = Game::At<void*>(node, Game::MBNode_MultiBound);
	void* shape = mb ? Game::At<void*>(mb, Game::MB_Shape) : nullptr;
	const uint16_t children = Game::At<uint16_t>(node, Game::Node_ChildSize);
	if (!shape || !children || *(uintptr_t*)shape != Game::Vtbl_MBAABB) {
		if (kDevTools && gFrame % 8 == 0) TallyMultiBound(node, shape);
		return false;
	}
	const float* c = &Game::At<float>(shape, Game::MBAABB_Center);
	const float* h = &Game::At<float>(shape, Game::MBAABB_HalfExtents);
	const bool sane = h[0] > 0.0f && h[1] > 0.0f && h[2] >= 0.0f && h[0] < 1.0e6f && h[1] < 1.0e6f && h[2] < 1.0e6f;
	void* child = *Game::At<void**>(node, Game::Node_ChildBase);
	// A land quadrant: 2048 x 2048, its land strips as the only child.
	if (sane && children == 1 && h[0] == 1024.0f && h[1] == 1024.0f && child && IsKindOf(child, Game::RTTI_NiGeometry)) {
		gStats.landQuads += 1;
		if (!gLandOn) return false;
		if (!ShapeInView(shape, culler)) { gStats.mbOutside += 1; return false; }
		return CollectBoxed(node, c, h, kBoxLand);
	}
	// A grass group: one TallGrassTriStrips child, directly under the scene's "Grass" node.
	if (sane && children == 1 && child && NameIs(Game::At<void*>(node, Game::Ni_Parent), "Grass") &&
		strcmp(RttiName(child), "TallGrassTriStrips") == 0) {
		gStats.grassSeen += 1;
		if (!gGrassOn) return false;
		if (!ShapeInView(shape, culler)) { gStats.mbOutside += 1; return false; }
		return CollectBoxed(node, c, h, kBoxGrass);
	}
	// A distant terrain LOD chunk: somewhere below the scene's "LandLOD" node (a few levels at most).
	bool lod = false;
	void* p = Game::At<void*>(node, Game::Ni_Parent);
	for (int i = 0; p && i < 4 && !lod; ++i, p = Game::At<void*>(p, Game::Ni_Parent)) lod = NameIs(p, "LandLOD");
	if (sane && lod) {
		gStats.lodSeen += 1;
		if (!gSet.lodTerrain || !gLandOn) return false;
		if (!ShapeInView(shape, culler)) { gStats.mbOutside += 1; return false; }
		return CollectBoxed(node, c, h, kBoxLod);
	}
	if (kDevTools && gFrame % 8 == 0) TallyMultiBound(node, shape);
	return false;
}

static bool SafeCollectLand(void* node, void* culler) {
	__try { return CollectLand(node, culler); }
	__except (EXCEPTION_EXECUTE_HANDLER) { OnFault("collecting land"); return false; }
}

static bool SafeCollect(void* node, bool tree) {
	__try { return Collect(node, tree); }
	__except (EXCEPTION_EXECUTE_HANDLER) { OnFault("collecting"); return false; }
}

// ---------------------------------------------------------------------------------------------------------------
// Queries (after the main opaque pass)

static void ReleaseAll() {
	gPending.clear();
	gModelBoxes.clear();
	for (auto& entry : gRecords) if (entry.second.query) entry.second.query->Release();
	gRecords.clear();
	for (auto& entry : gLamps) {
		if (entry.second.rec.query) entry.second.rec.query->Release();
		entry.second.rec = Record{};
	}
	gLampCandidates.clear();
	for (IDirect3DQuery9* q : gFreeQueries) q->Release();
	gFreeQueries.clear();
	gCandidates.clear();
}

static constexpr DWORD kFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

// The few device states the box draws need, each read first and put back to exactly that value (see the header).
struct MinimalStates {
	static constexpr int kStates = 9;
	IDirect3DDevice9* device = nullptr;
	D3DRENDERSTATETYPE type[kStates] = { D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2,
		D3DRS_COLORWRITEENABLE3, D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE };
	DWORD saved[kStates] = {}, now[kStates] = {};
	DWORD cull = 0, fvf = 0;
	IDirect3DVertexShader9* vs = nullptr;
	IDirect3DPixelShader9* ps = nullptr;
	IDirect3DVertexDeclaration9* decl = nullptr;
	IDirect3DVertexBuffer9* vb = nullptr;
	UINT offset = 0, stride = 0;
	bool active = false;
	// With alpha testing on, the boxes' alpha must be their own vertex colour (255), not whatever texture stage 0 holds:
	// stage 0 alpha = diffuse, stage 1 off. Alpha testing itself is never touched.
	bool alphaOn = false;
	DWORD alphaOp0 = 0, alphaArg0 = 0, colorOp1 = 0;

	void Set(int i, DWORD value) {
		if (now[i] != value) { device->SetRenderState(type[i], value); now[i] = value; }
	}

	// Whether a fully opaque pixel (alpha 255) passes the alpha comparison func(255, ref).
	static bool OpaquePasses(DWORD func, DWORD ref) {
		switch (func) {
		case D3DCMP_ALWAYS: case D3DCMP_GREATEREQUAL: return true;
		case D3DCMP_GREATER: case D3DCMP_NOTEQUAL: return ref < 255;
		case D3DCMP_EQUAL: case D3DCMP_LESSEQUAL: return ref == 255;
		default: return false;   // LESS, NEVER
		}
	}

	// False (and nothing changed) when alpha testing is on with a comparison that would cut fully opaque box pixels.
	// Alpha testing itself is never written (see the header).
	bool Enter(IDirect3DDevice9* d, bool depthGrows) {
		DWORD alphaTest = FALSE, func = D3DCMP_ALWAYS, ref = 0;
		if (FAILED(d->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest))) return false;
		alphaOn = alphaTest != FALSE;
		if (alphaOn && (FAILED(d->GetRenderState(D3DRS_ALPHAFUNC, &func)) || FAILED(d->GetRenderState(D3DRS_ALPHAREF, &ref)) ||
			!OpaquePasses(func, ref & 0xFF)))
			return false;
		if (alphaOn && (FAILED(d->GetTextureStageState(0, D3DTSS_ALPHAOP, &alphaOp0)) ||
			FAILED(d->GetTextureStageState(0, D3DTSS_ALPHAARG1, &alphaArg0)) ||
			FAILED(d->GetTextureStageState(1, D3DTSS_COLOROP, &colorOp1))))
			return false;
		device = d;
		for (int i = 0; i < kStates; ++i) { device->GetRenderState(type[i], &saved[i]); now[i] = saved[i]; }
		device->GetRenderState(D3DRS_CULLMODE, &cull);
		device->GetVertexShader(&vs);
		device->GetPixelShader(&ps);
		device->GetVertexDeclaration(&decl);
		device->GetFVF(&fvf);
		device->GetStreamSource(0, &vb, &offset, &stride);
		active = true;
		for (int i = 0; i < 4; ++i) Set(i, 0);            // no colour writes
		Set(4, D3DZB_TRUE);
		Set(5, FALSE);                                      // no depth writes
		const DWORD z = now[6];
		const bool sameWay = depthGrows ? (z == D3DCMP_LESS || z == D3DCMP_LESSEQUAL) : (z == D3DCMP_GREATER || z == D3DCMP_GREATEREQUAL);
		if (!sameWay) Set(6, depthGrows ? D3DCMP_LESSEQUAL : D3DCMP_GREATEREQUAL);
		Set(7, FALSE);
		Set(8, FALSE);
		if (cull != D3DCULL_NONE) device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		device->SetVertexShader(nullptr);
		device->SetPixelShader(nullptr);
		device->SetFVF(kFVF);
		if (alphaOn) {
			if (alphaOp0 != D3DTOP_SELECTARG1) device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
			if (alphaArg0 != D3DTA_DIFFUSE) device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
			if (colorOp1 != D3DTOP_DISABLE) device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		}
		return true;
	}

	void Leave() {
		if (!active) return;
		active = false;
		if (alphaOn) {
			if (colorOp1 != D3DTOP_DISABLE) device->SetTextureStageState(1, D3DTSS_COLOROP, colorOp1);
			if (alphaArg0 != D3DTA_DIFFUSE) device->SetTextureStageState(0, D3DTSS_ALPHAARG1, alphaArg0);
			if (alphaOp0 != D3DTOP_SELECTARG1) device->SetTextureStageState(0, D3DTSS_ALPHAOP, alphaOp0);
		}
		if (decl) device->SetVertexDeclaration(decl); else device->SetFVF(fvf);
		device->SetStreamSource(0, vb, offset, stride);
		device->SetVertexShader(vs);
		device->SetPixelShader(ps);
		if (cull != D3DCULL_NONE) device->SetRenderState(D3DRS_CULLMODE, cull);
		for (int i = 0; i < kStates; ++i) Set(i, saved[i]);
		if (decl) { decl->Release(); decl = nullptr; }
		if (vb) { vb->Release(); vb = nullptr; }
		if (vs) { vs->Release(); vs = nullptr; }
		if (ps) { ps->Release(); ps = nullptr; }
	}
};
static MinimalStates gStates;

// This frame's tests: boxes projected first (8 corners each), drawn after.
// cls -1 = a lamp. A plain box is its 8 projected corners (expanded into 36 vertices when drawn); a clipped one is
// its triangle list (count vertices).
struct BatchItem { Record* rec; int8_t cls; bool clipped; uint32_t first, count; };
static std::vector<BatchItem> gBatch;
static std::vector<OcclusionMath::ScreenVertex> gBatchVerts;

static IDirect3DQuery9* AcquireQuery(IDirect3DDevice9* device) {
	if (!gFreeQueries.empty()) { IDirect3DQuery9* q = gFreeQueries.back(); gFreeQueries.pop_back(); return q; }
	IDirect3DQuery9* q = nullptr;
	if (FAILED(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &q))) return nullptr;
	return q;
}

// Draws this frame's boxes, each inside its own query. Returns the objects (not lamps) drawn.
static int DrawBatch(IDirect3DDevice9* device) {
	const UINT n = (UINT)gBatch.size();
	OcclusionMath::ScreenVertex tri[36];
	int objects = 0;
	for (UINT k = 0; k < n; ++k) {
		const BatchItem& b = gBatch[k];
		Record& rec = *b.rec;
		if (!rec.query) rec.query = AcquireQuery(device);
		if (!rec.query) break;
		rec.query->Issue(D3DISSUE_BEGIN);
		if (b.clipped) {
			device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, b.count / 3, &gBatchVerts[b.first], sizeof(OcclusionMath::ScreenVertex));
		} else {
			OcclusionMath::ExpandBox(&gBatchVerts[b.first], tri);
			device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 12, tri, sizeof(OcclusionMath::ScreenVertex));
		}
		rec.query->Issue(D3DISSUE_END);
		rec.pending = true;
		rec.issued = gFrame;
		gPending.push_back(&rec);
		if (b.cls < 0) { gStats.lampsTested += 1; continue; }
		for (int i = 0; i < 3; ++i) { rec.tmn[i] = rec.mn[i]; rec.tmx[i] = rec.mx[i]; }
		++objects;
		if (b.cls == Scheduler::kCovered) gStats.issuedCovered += 1;
		else if (b.cls == Scheduler::kNew) gStats.issuedNew += 1;
		else gStats.issuedVisible += 1;
	}
	return objects;
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

	// 1. Answers of earlier frames, oldest first. The card answers in the order the queries were issued, so after the
	// first one that is not ready yet the rest wait for the next frame without being asked.
	LARGE_INTEGER t0 = {}, t1 = {};
	if (kDevTools) QueryPerformanceCounter(&t0);
	size_t kept = 0;
	bool waiting = false;
	for (size_t k = 0; k < gPending.size(); ++k) {
		Record& rec = *gPending[k];
		if (!rec.pending) continue;   // reset meanwhile (another object at that address, or a lamp that moved)
		if (waiting) { gPending[kept++] = &rec; gStats.pollsSkipped += 1; continue; }
		DWORD pixels = 0;
		const HRESULT hr = rec.query->GetData(&pixels, sizeof(pixels), 0);
		if (hr == S_OK) {
			rec.pending = false;
			rec.answered = true;
			rec.answerFrame = rec.issued;
			for (int i = 0; i < 3; ++i) { rec.amn[i] = rec.tmn[i]; rec.amx[i] = rec.tmx[i]; }
			rec.pixels = pixels;
			if (pixels) {
				// Pixels from a grown box: maybe only the margin. Drawn now, tested next frame (see the header).
				if (rec.issuedGrown && (rec.zeroStreak > 0 || rec.afterGrown)) {
					if (!rec.afterGrown) rec.streakBeforeGrown = rec.zeroStreak;   // keep the first one through a run of them
					rec.afterGrown = true;
					gStats.revealShown += 1;
				} else rec.afterGrown = false;
				rec.zeroStreak = 0;
			} else if (rec.afterGrown) {
				rec.zeroStreak = (uint8_t)min(rec.streakBeforeGrown + 1, 255);   // covered after all: as before
				rec.afterGrown = false;
				gStats.revealRestored += 1;
			} else {
				rec.zeroStreak = (uint8_t)min(rec.zeroStreak + 1, 255);
			}
			gStats.answered += 1;
			gStats.ageSum += gFrame - rec.issued;
			if (!pixels) gStats.zeroAnswers += 1;
		} else if (hr == S_FALSE) {
			gStats.notReady += 1;
			gPending[kept++] = &rec;
			waiting = true;
		} else {
			gStats.failed += 1;
			if (hr == D3DERR_DEVICELOST) { ReleaseAll(); return; }   // clears gPending: stop here
			rec.pending = false;
			rec.query->Release();   // never reused: after a device reset it may be dead
			rec.query = nullptr;
		}
	}
	gPending.resize(kept);
	if (kDevTools) {
		QueryPerformanceCounter(&t1);
		gStats.pollUs += (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)gQpcFreq.QuadPart;
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

	if (canIssue && !gCandidates.empty()) {
		gDepthGrows = OcclusionMath::DepthGrowsWithDistance(&proj._11);
		// Boxes are only tested when they can be drawn: an undrawn query would answer 0 pixels and hide its object.
		if (!gStates.Enter(device, gDepthGrows)) { gStats.alphaTestFrames += 1; canIssue = false; }
		else if (gStates.alphaOn) gStats.alphaTestedFrames += 1;
	}
	if (canIssue && !gCandidates.empty()) {
		// Who is tested this frame, in what order (Scheduler.h).
		static std::vector<Scheduler::Item> order;
		order.clear();
		for (uint32_t i = 0; i < (uint32_t)gCandidates.size(); ++i) {
			const Record& rec = *gCandidates[i].rec;
			if (rec.pending) continue;
			const uint32_t since = gFrame - rec.issued;
			const Scheduler::Class cls = rec.afterGrown ? Scheduler::kCovered :
				Scheduler::Classify(rec.answered, rec.zeroStreak, since, (uint32_t)gSet.visibleRetest);
			if (cls == Scheduler::kNotDue) { gStats.notDue += 1; continue; }
			order.push_back({ (uint8_t)cls, since, i });
		}
		// The order only matters when not everyone fits the budget (Scheduler.h).
		// Over budget, only which ones are tested matters, not their order: pick the most urgent iMaxQueriesPerFrame.
		const size_t top = (size_t)gSet.maxQueries;
		if (order.size() > top) { Scheduler::SelectTop(order, top); gStats.sorted += 1; }
		LARGE_INTEGER t2 = {}, t3 = {}, t4 = {};
		if (kDevTools) {
			QueryPerformanceCounter(&t2);
			gStats.planUs += (double)(t2.QuadPart - t1.QuadPart) * 1e6 / (double)gQpcFreq.QuadPart;
		}

		D3DVIEWPORT9 vp = {};
		device->GetViewport(&vp);
		OcclusionMath::ScreenVertex screen[36];
		int issued = 0;

		// This frame's boxes, projected into the batch: lamps first (few, and each one decides a whole lamp's passes),
		// then objects in the scheduler's order up to the budget. Drawn together below.
		gBatch.clear();
		gBatchVerts.clear();
		// How far this frame's camera move can slide an occluder's edge (see the header); the boxes of things last seen
		// covered grow by that much, so they show pixels the frame before they come out.
		float grow = 0.0f;
		if (gRevealOn && gSet.revealDepth > 0.0f && gFrame > 1) {
			const float* last = gCamPos[(gFrame - 1) % kRing];
			const float dx = gNowPos[0] - last[0], dy = gNowPos[1] - last[1], dz = gNowPos[2] - last[2];
			const float move = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (move >= kRevealMinMove)
				grow = OcclusionMath::RevealPixels((float)vp.Width * 0.5f, proj._11, move, gSet.revealDepth, (float)vp.Width * 0.25f);
			if (grow > 0.0f) {
				gStats.revealFrames += 1;
				gStats.revealPx += grow;
				if (grow > gStats.revealMaxPx) gStats.revealMaxPx = grow;
			}
		}
		// Boxes reaching behind the camera or far past a screen edge are cut to what can be drawn (OcclusionMath::
		// ProjectCornersClipped); a box with nothing in front of the camera is not tested.
		auto add = [&](Record& rec, const float (*corners)[3], int8_t cls) {
			const size_t at = gBatchVerts.size();
			const bool plain = OcclusionMath::CornersUnclipped(corners, posAdjust, &view._11, &proj._11);
			uint32_t count = 8;
			if (plain) {
				gBatchVerts.resize(at + 8);
				OcclusionMath::ProjectCornerPoints(corners, posAdjust, &view._11, &proj._11, (float)vp.X, (float)vp.Y,
					(float)vp.Width, (float)vp.Height, vp.MinZ, vp.MaxZ, 0xFFFFFFFF, &gBatchVerts[at]);
			} else {
				gBatchVerts.resize(at + OcclusionMath::kMaxClippedVertices);
				count = (uint32_t)OcclusionMath::ProjectCornersClipped(corners, posAdjust, &view._11, &proj._11, (float)vp.X,
					(float)vp.Y, (float)vp.Width, (float)vp.Height, vp.MinZ, vp.MaxZ, 0xFFFFFFFF, &gBatchVerts[at]);
				gBatchVerts.resize(at + count);
				if (!count) return false;
				gStats.clippedBoxes += 1;
			}
			rec.issuedGrown = grow > 0.0f && rec.answered && (rec.zeroStreak > 0 || rec.afterGrown);
			if (rec.issuedGrown) {
				if (plain) OcclusionMath::GrowOnScreen(&gBatchVerts[at], grow);
				else OcclusionMath::GrowOnScreenN(&gBatchVerts[at], (int)count, grow);
				gStats.revealGrown += 1;
			}
			gBatch.push_back({ &rec, cls, !plain, (uint32_t)at, count });
			return true;
		};
		for (Lamp* lamp : gLampCandidates) {
			Record& rec = lamp->rec;
			if (rec.pending || lamp->nearCamera) continue;
			// A lamp whose reach was seen is tested again only every iVisibleRetestFrames frames (it can only stay lit
			// a little longer); hidden and new ones every frame, so a hidden lamp comes back on at once.
			if (!rec.afterGrown && Scheduler::Classify(rec.answered, rec.zeroStreak, gFrame - rec.issued,
				(uint32_t)gSet.visibleRetest) == Scheduler::kNotDue) { gStats.lampsWaiting += 1; continue; }
			float corners[8][3];
			OcclusionMath::AxisCorners(rec.mn, rec.mx, corners);
			add(rec, corners, -1);
		}
		for (size_t k = 0; k < order.size(); ++k) {
			if (issued >= gSet.maxQueries) { gStats.budgetSkips += 1; continue; }
			// Budget left after the most urgent ones (some reached behind the camera): the next ones in full order, as
			// a full sort would have given them.
			if (k == top) std::sort(order.begin() + k, order.end(), Scheduler::Before);
			const Scheduler::Item& item = order[k];
			Record& rec = *gCandidates[item.index].rec;
			if (!add(rec, rec.corner, (int8_t)item.cls)) { rec.issued = gFrame; gStats.behindCamera += 1; continue; }   // drawn
			++issued;
		}
		if (kDevTools) {
			QueryPerformanceCounter(&t3);
			gStats.prepUs += (double)(t3.QuadPart - t2.QuadPart) * 1e6 / (double)gQpcFreq.QuadPart;
		}
		gStats.issued += DrawBatch(device);
		if (kDevTools) {
			QueryPerformanceCounter(&t4);
			gStats.drawUs += (double)(t4.QuadPart - t3.QuadPart) * 1e6 / (double)gQpcFreq.QuadPart;
		}

		if (kDevTools && gSet.debugBoxes) {
			gStates.Set(0, 0xF);   // colour into the first target only
			for (const Candidate& c : gCandidates) {
				const Record& rec = *c.rec;
				if (!rec.answered) continue;
				gStates.Set(4, c.wouldHide ? D3DZB_FALSE : D3DZB_TRUE);
				if (OcclusionMath::ProjectCorners(rec.corner, posAdjust, &view._11, &proj._11, (float)vp.X, (float)vp.Y,
					(float)vp.Width, (float)vp.Height, vp.MinZ, vp.MaxZ, c.wouldHide ? 0xFFFF2020 : 0xFF20FF20, screen))
					device->DrawPrimitiveUP(D3DPT_LINELIST, 18, screen, sizeof(OcclusionMath::ScreenVertex));
			}
		}
	}
	gStates.Leave();

	// 3. Forget objects not drawn for a while.
	if (gFrame % 64 == 0) {
		for (auto it = gRecords.begin(); it != gRecords.end();) {
			if (gFrame - it->second.lastSeen > 600 && !it->second.pending) {
				if (it->second.query) gFreeQueries.push_back(it->second.query);
				it = gRecords.erase(it);
			} else ++it;
		}
	}
	if (gFrame % 64 == 32) {
		for (auto it = gLamps.begin(); it != gLamps.end();) {
			if (gFrame - it->second.seenFrame > 600 && !it->second.overrideActive && !it->second.rec.pending) {
				if (it->second.rec.query) gFreeQueries.push_back(it->second.rec.query);
				it = gLamps.erase(it);
			} else ++it;
		}
	}
	gStats.records += (double)gRecords.size();
}

static void SafeRunQueries(void* camera) {
	__try { RunQueries(camera); }
	__except (EXCEPTION_EXECUTE_HANDLER) {
		gStates.Leave();
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
	Log("--- %.0f frames, player at (%.0f, %.0f, %.0f) in cell %08X \"%s\" | actors %s, oriented boxes %s, trees %s, doors/"
		"activators %s", gStats.frames, px, py, pz, cellID, name, gActorsOn ? "on" : "off", gOrientedOn ? "on" : "off",
		gTreesOn ? "on" : "off", gActivatorsOn ? "on" : "off");
	Log("  objects drawn by the main view (per frame): %.0f; not candidates: %.1f no reference, %.1f actors, %.1f doors/lights/"
		"activators/effects (when not tested themselves), %.1f distant LOD, %.1f imposters (bImposters=0), %.1f terminal screens, "
		"%.1f disabled/invisible, %.1f small, %.1f hidden by the game, %.1f near the camera", gStats.topFade / f,
		gStats.noRef / f, gStats.actors / f, gStats.kinds / f, gStats.lod / f, gStats.lodImposter / f, gStats.lodMenu / f,
		gStats.lodInvisible / f, gStats.smallObj / f, gStats.culledFlag / f, gStats.nearSkip / f);
	Log("  imposters (shown at any distance, %s): %.1f per frame drawn by the main view, %.1f tested (%.0f shapes), %.1f hidden "
		"(%.0f shapes); counted with the objects above and below", gSet.imposters ? "tested" : "not tested", gStats.impSeen / f,
		gStats.impCands / f, gStats.impGeoms / f, gStats.impHidden / f, gStats.impHiddenGeoms / f);
	if (gStats.topFade == 0)
		Log("  Warning: no object reached the BSFadeNode hook in this block (another plugin replaced that vtable slot later?)");
	Log("  hiding %s: %.1f objects = %.0f shapes not drawn per frame | frame time: %.2f ms with hiding on (%.0f frames), %.2f ms "
		"with it off (%.0f frames)", gCullingOn ? "ON" : "OFF", gStats.hidden / f, gStats.hiddenGeoms / f,
		gStats.onFrames > 0 ? gStats.onMs / gStats.onFrames : 0.0, gStats.onFrames,
		gStats.offFrames > 0 ? gStats.offMs / gStats.offFrames : 0.0, gStats.offFrames);
	auto logMoving = [&](const char* what, bool on, const Stats::Moving& m) {
		Log("  %s (%s): %.1f per frame drawn by the main view, %.1f with nothing to draw, %.1f near the camera, %.1f tested (%.0f "
			"shapes) | covered %.1f: hidden %.1f (%.0f shapes), moved out of the tested box %.2f, camera guard %.1f, answer too old "
			"%.2f", what, on ? "on" : "off", m.seen / f, m.noShapes / f, m.nearCam / f, m.cands / f, m.geoms / f, m.covered / f,
			m.hidden / f, m.hiddenGeoms / f, m.moved / f, m.guard / f, m.old / f);
	};
	logMoving("actors", gActorsOn, gStats.actor);
	logMoving("doors, activators, terminals", gActivatorsOn, gStats.door);
	Log("  land (%s): %.1f multibound nodes per frame drawn by the main view, %.1f land quadrants, %.1f near the camera, %.1f "
		"tested | covered %.1f: hidden %.1f, camera guard / answer too old %.1f", gLandOn ? "on" : "off", gStats.landSeen / f,
		gStats.landQuads / f, gStats.landNear / f, gStats.landCands / f, gStats.landCovered / f, gStats.landHidden / f,
		gStats.landGuard / f);
	Log("  distant terrain LOD (%s): %.1f chunks per frame drawn by the main view, %.1f near the camera, %.1f tested | covered "
		"%.1f: hidden %.1f, camera guard / answer too old %.1f", gSet.lodTerrain && gLandOn ? "on" : "off", gStats.lodSeen / f,
		gStats.lodNear / f, gStats.lodCands / f, gStats.lodCovered / f, gStats.lodHidden / f, gStats.lodGuard / f);
	Log("  grass (%s): %.1f groups per frame drawn by the main view, %.1f near the camera, %.1f tested | covered %.1f: hidden "
		"%.1f, camera guard / answer too old %.1f | land, LOD and grass nodes outside the view %.1f", gGrassOn ? "on" : "off",
		gStats.grassSeen / f, gStats.grassNear / f, gStats.grassCands / f, gStats.grassCovered / f, gStats.grassHidden / f,
		gStats.grassGuard / f, gStats.mbOutside / f);
	if (!gOtherMultiBounds.empty()) {
		std::vector<std::pair<double, std::string>> kinds;
		for (auto& k : gOtherMultiBounds) kinds.push_back({ k.second, k.first });
		std::sort(kinds.begin(), kinds.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
		Log("  other multibound nodes, left alone (per frame, sampled every 8th frame; %u kinds):", (unsigned)kinds.size());
		for (size_t i = 0; i < kinds.size() && i < 12; ++i) Log("    %.1f  %s", kinds[i].first / f, kinds[i].second.c_str());
		gOtherMultiBounds.clear();
	}
	{
		using namespace AnimCost;
		const double us = 1e6 / (double)gQpcFreq.QuadPart / f;
		auto step = [&](Step s) {
			static char text[kSteps][200];
			const Sum& m = gSum[s];
			if (!gHooked[s]) { snprintf(text[s], sizeof text[s], "not hooked"); return (const char*)text[s]; }
			snprintf(text[s], sizeof text[s], "%.0f / %.0f / %.0f us (%.1f / %.1f / %.1f actors)", m.ticks[kDrawn] * us,
				m.ticks[kHidden] * us, m.ticks[kOutside] * us, m.calls[kDrawn] / f, m.calls[kHidden] / f, m.calls[kOutside] / f);
			return (const char*)text[s];
		};
		Log("  actor animation, CPU per frame summed over all threads (not the player), drawn / hidden by us / outside the "
			"main view: sequences + pose (AI thread) %s | movement + other controllers (parallel tasks) %s | bone transforms "
			"(after the AI join) %s", step(kUpdate), step(kPose), step(kBones));
		Log("  actor bones (%s, every %d frames beyond %.0f units): left for a later frame %.1f hidden + %.1f outside the view "
			"per frame", gAnimThrottleOn && gSet.animInterval > 1 ? "throttled" : "every frame", gSet.animInterval,
			gSet.animNearDistance, gSkipped[kHidden] / f, gSkipped[kOutside] / f);
		for (volatile LONG64& k : gSkipped) InterlockedExchange64(&k, 0);
		for (Sum& m : gSum)
			for (int h = 0; h < kClasses; ++h) { InterlockedExchange64(&m.ticks[h], 0); InterlockedExchange64(&m.calls[h], 0); }
		Prune(gFrame);
	}
	if (CellCost::gHooked) {
		using namespace CellCost;
		using AnimCost::kDrawn; using AnimCost::kHidden; using AnimCost::kOutside;
		const double us = 1e6 / (double)gQpcFreq.QuadPart / f;
		const double objUs = (gObjTicks[kDrawn] + gObjTicks[kHidden] + gObjTicks[kOutside]) * us;
		Log("  cell update (AI thread, per frame): Havok step %.0f us (%.1f world steps) | cell updates %.0f us (%.1f cells) = "
			"interior Havok %.0f + animated objects %.0f + rest %.0f us | animated objects drawn / hidden by us / outside the "
			"main view: %.0f / %.0f / %.0f us (%.1f / %.1f / %.1f objects)", gPhysTicks * us, gPhysCalls / f, gCellTicks * us,
			gCellCalls / f, gPhysInCellTicks * us, objUs, gCellTicks * us - gPhysInCellTicks * us - objUs,
			gObjTicks[kDrawn] * us, gObjTicks[kHidden] * us, gObjTicks[kOutside] * us, gObjCalls[kDrawn] / f,
			gObjCalls[kHidden] / f, gObjCalls[kOutside] / f);
		for (volatile LONG64* k : { &gPhysTicks, &gPhysCalls, &gPhysInCellTicks, &gCellTicks, &gCellCalls }) InterlockedExchange64(k, 0);
		for (int c = 0; c < AnimCost::kClasses; ++c) { InterlockedExchange64(&gObjTicks[c], 0); InterlockedExchange64(&gObjCalls[c], 0); }
		auto part = [&](Part p) { return gPartOrig[p] ? gPartTicks[p] * us : -1.0; };
		Log("  cell update parts (AI thread, per frame; -1 = not hooked): Havok lock wait %.0f us | exterior cells incl. Havok step "
			"%.0f us | queued parallel tasks %.0f us | collision damage %.0f us | projectiles %.0f us", part(kLock), part(kGrid),
			part(kTasks), part(kDamage), part(kProjectiles));
		for (int p = 0; p < kParts; ++p) { InterlockedExchange64(&gPartTicks[p], 0); InterlockedExchange64(&gPartCalls[p], 0); }
		Prune(gFrame);
	}
	Log("  trees (SpeedTree, tree culling %s): %.1f per frame drawn by the main view, %.1f tested (%.0f shapes), %.1f hidden "
		"(%.0f shapes); counted with the objects above and below", gTreesOn ? "on" : "off", gStats.treeSeen / f, gStats.treeCands / f,
		gStats.treeGeoms / f, gStats.treeHidden / f, gStats.treeHiddenGeoms / f);
	Log("  lamps lit in the main view: %.1f per frame (lighting %.0f shapes); tested %.1f (%.1f visible ones waiting their turn), "
		"near the camera %.1f; reach fully hidden %.1f = %.0f lit shapes (%.1f%%) | switched off %s: %.2f per frame newly, shapes "
		"re-marked %.2f", gStats.lampsOn / f, gStats.lampShapes / f, gStats.lampsTested / f, gStats.lampsWaiting / f,
		gStats.lampsNear / f, gStats.lampsCovered / f, gStats.lampShapesCovered / f,
		gStats.lampShapes > 0 ? 100.0 * gStats.lampShapesCovered / gStats.lampShapes : 0.0, gSet.lampCulling ? "on" : "off (measuring)",
		gStats.lampsOff / f, gStats.lampFlags / f);
	Log("  candidates: %.1f per frame (%.0f shapes); covered %.1f (%.1f%%) = %.0f shapes (%.1f%% of candidate shapes)",
		cand, gStats.candGeoms / f, gStats.wouldHide / f, cand > 0 ? 100.0 * gStats.wouldHide / gStats.candidates : 0.0,
		gStats.hideGeoms / f, gStats.candGeoms > 0 ? 100.0 * gStats.hideGeoms / gStats.candGeoms : 0.0);
	Log("  covered but not used: %.1f camera guard, %.1f answer too old; %.1f with no answer yet", gStats.guardBlocked / f,
		gStats.tooOld / f, gStats.noAnswer / f);
	Log("  boxes built in this block: %.0f (%.0f measured again with the vertex budget spent); %.0f oriented, %.0f tighter along "
		"the world axes, %.0f animated (old box kept); shapes measured from their vertices %.0f, from bound spheres %.0f (vertices "
		"freed after upload, skinned, particles), odd data %.0f | average box volume %.0f%% of the old boxes",
		gStats.boxesBuilt, gStats.boxesPartial, gStats.boxesOriented, gStats.boxesAxisTighter, gStats.boxesAnimated,
		gStats.vertexShapes, gStats.sphereShapes, gStats.oddShapes,
		gStats.boxesBuilt > 0 ? 100.0 * gStats.volumeRatio / gStats.boxesBuilt : 100.0);
	Log("  queries per frame: %.1f issued (%.1f covered, %.1f new, %.1f visible; %.1f visible waiting their turn), %.1f answered "
		"(%.1f zero pixels), average age %.2f frames, %.1f not ready, %.2f failed, %.1f over budget | records %.0f",
		gStats.issued / f, gStats.issuedCovered / f, gStats.issuedNew / f, gStats.issuedVisible / f, gStats.notDue / f,
		gStats.answered / f, gStats.zeroAnswers / f,
		gStats.answered > 0 ? gStats.ageSum / gStats.answered : 0.0, gStats.notReady / f, gStats.failed / f, gStats.budgetSkips / f,
		gStats.records / f);
	Log("  growing boxes while moving (%s, fRevealDepth %.0f): camera moving in %.0f%% of frames, boxes grown by %.0f px on "
		"average (most %.0f px), %.1f covered boxes grown per frame | grown boxes that showed pixels %.2f per frame, of those "
		"covered again the next frame %.2f", gRevealOn ? "on" : "off", gSet.revealDepth, 100.0 * gStats.revealFrames / f,
		gStats.revealFrames > 0 ? gStats.revealPx / gStats.revealFrames : 0.0, gStats.revealMaxPx, gStats.revealGrown / f,
		gStats.revealShown / f, gStats.revealRestored / f);
	Log("  cost per frame: %.0f us query pass (CPU) = %.0f reading answers (%.1f left for the next frame unasked) + %.0f "
		"planning (over budget in %.0f%% of frames) + %.0f preparing boxes + %.0f drawing the tests | frames skipped: %.0f camera "
		"mismatch, %.0f "
		"no depth buffer, %.0f alpha test that would cut the boxes | tested with alpha testing on (full-alpha boxes) %.0f frames | "
		"boxes cut at the camera or screen edge %.1f, with nothing in front of the camera %.1f per frame | identity resets %.0f",
		gStats.queryUs / f, gStats.pollUs / f, gStats.pollsSkipped / f, gStats.planUs / f, 100.0 * gStats.sorted / f,
		gStats.prepUs / f, gStats.drawUs / f,
		gStats.camMismatch, gStats.noDepth, gStats.alphaTestFrames, gStats.alphaTestedFrames,
		gStats.clippedBoxes / f, gStats.behindCamera / f, gStats.identityResets);
	gStats = Stats{};
}

// ---------------------------------------------------------------------------------------------------------------
// Hooks

// Marks every shape the lamp lights to rebuild its lamp list (as TestFrustumCull does when a lamp turns on or off).
static void MarkLampShapes(void* light) {
	struct Item { Item* next; Item* prev; void* element; };
	const Item* item = Game::At<const Item*>(light, Game::SSL_Geometries);
	for (int n = 0; item && n < 100000; ++n, item = item->next) {
		void* geo = item->element;
		void* shade = geo ? Game::At<void*>(geo, Game::Geo_ShadeProperty) : nullptr;
		if (!shade) continue;
		const int type = Game::At<int>(shade, Game::Shade_Type);
		if (type >= 1 && type <= 12) Game::At<bool>(shade, Game::Shade_LightListChanged) = true;
	}
	gStats.lampFlags += 1;
}

static __declspec(noinline) void LampNote(void* light, void* culler) {
	if (!Game::At<bool>(light, Game::SSL_PointLight)) return;
	void* niLight = Game::At<void*>(light, Game::SSL_Light);
	if (!niLight) return;
	const float radius = Game::At<float>(niLight, Game::Light_SpecR);
	if (!(radius > 0.0f) || radius > 100000.0f) return;
	void* camera = Game::At<void*>(culler, Game::Culler_Camera);
	if (!camera) return;
	const float* camPos = &Game::At<float>(camera, Game::Ni_WorldTranslate);
	const float* camRot = &Game::At<float>(camera, Game::Ni_WorldRotate);
	uint16_t& cull = Game::At<uint16_t>(light, Game::SSL_FrustumCull);

	static uint32_t listFrame = 0;
	if (listFrame != gFrame) { gLampCandidates.clear(); listFrame = gFrame; }   // a new frame's list, even if the last query pass did not run
	Lamp& lamp = gLamps[light];
	if (lamp.seenFrame != gFrame) {   // first main-view test this frame: refresh the box, decide once
		const float* c = &Game::At<float>(niLight, Game::Ni_WorldTranslate);
		const float reach = radius * 1.05f + 32.0f;
		float mn[3], mx[3];
		for (int i = 0; i < 3; ++i) { mn[i] = c[i] - reach; mx[i] = c[i] + reach; }
		Record& rec = lamp.rec;
		bool moved = lamp.light != niLight;
		for (int i = 0; i < 3 && !moved; ++i) moved = std::fabs(rec.mn[i] - mn[i]) > 8.0f || std::fabs(rec.mx[i] - mx[i]) > 8.0f;
		if (moved) {   // another lamp at a reused address, or it moved: old answers are not about this reach
			if (rec.pending && rec.query) { rec.query->Release(); rec.query = nullptr; }
			rec.pending = false; rec.answered = false; rec.zeroStreak = 0; rec.afterGrown = false;
			lamp.light = niLight;
		}
		for (int i = 0; i < 3; ++i) { rec.mn[i] = mn[i]; rec.mx[i] = mx[i]; }
		lamp.seenFrame = gFrame;
		lamp.candidate = cull != 255;   // lit in the main view by the game (and NVR)
		lamp.nearCamera = OcclusionMath::DistanceToBox(camPos, mn, mx) < gSet.nearDistance;
		lamp.decision = false;
		if (lamp.candidate) {
			const double shapes = (double)Game::At<uint32_t>(light, Game::SSL_Geometries + 8);
			gStats.lampsOn += 1;
			gStats.lampShapes += shapes;
			gLampCandidates.push_back(&lamp);
			if (lamp.nearCamera) gStats.lampsNear += 1;
			else if (rec.answered && rec.zeroStreak >= 3) {
				bool tooOld;
				if (camera == gMainCamera ? AnswerUsable(rec, tooOld) : AnswerUsableAt(rec, tooOld, camPos, camRot)) {
					lamp.decision = true;
					gStats.lampsCovered += 1;
					gStats.lampShapesCovered += shapes;
				}
			}
		}
	}

	const bool apply = gSet.lampCulling && gCullingOn && lamp.decision && cull != 255;
	if (apply) {
		lamp.savedCull = cull;
		cull = 255;
		lamp.overrideActive = true;
		if (!lamp.hiddenApplied) gStats.lampsOff += 1;
	}
	if (apply != lamp.hiddenApplied) {
		MarkLampShapes(light);
		lamp.hiddenApplied = apply;
	}
}

static void SafeLampNote(void* light, void* culler) {
	__try { LampNote(light, culler); }
	__except (EXCEPTION_EXECUTE_HANDLER) { OnFault("the lamp test"); }
}

typedef void(__thiscall* LampTest_t)(void*, void*);
static LampTest_t OrigLampTest = nullptr;

static void __fastcall LampTestHook(void* light, void*, void* culler) {
	// Put back what the game and NVR left before we switched the lamp off, so their own bookkeeping sees its value.
	const bool mainThread = GetCurrentThreadId() == gMainThread;
	if (mainThread && !gLamps.empty()) {
		auto it = gLamps.find(light);
		if (it != gLamps.end() && it->second.overrideActive) {
			Game::At<uint16_t>(light, Game::SSL_FrustumCull) = it->second.savedCull;
			it->second.overrideActive = false;
		}
	}
	OrigLampTest(light, culler);
	if (mainThread && gSet.enabled && !gFaulted && culler && culler == gMainCuller)
		SafeLampNote(light, culler);
}

typedef void(__cdecl* Accumulate_t)(void*, void*, void*);
static Accumulate_t OrigAccumulate = nullptr;
typedef void(__cdecl* Opaque_t)(void*, void*);
static Opaque_t OrigOpaque = nullptr;
typedef void(__thiscall* OnVisible_t)(void*, void*);
static OnVisible_t OrigFadeOnVisible = nullptr;
static OnVisible_t OrigTreeOnVisible = nullptr;
static OnVisible_t OrigMultiBoundOnVisible = nullptr;

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
		gVertexBudget = kVertexBudget;
		ComputeGuard();
		gFadeDepth = 0;
		gInMain = true;
	}
	OrigAccumulate(camera, scene, culler);
	gInMain = false;
}

static void __fastcall FadeOnVisibleHook(void* node, void*, void* culler) {
	if (gInMain && culler == gMainCuller && GetCurrentThreadId() == gMainThread) {
		if (gFadeDepth == 0 && SafeCollect(node, false) && gCullingOn && !gFaulted) return;   // proven covered: not drawn
		++gFadeDepth;
		OrigFadeOnVisible(node, culler);
		--gFadeDepth;
		return;
	}
	OrigFadeOnVisible(node, culler);
}

static void __fastcall TreeOnVisibleHook(void* node, void*, void* culler) {
	if (gInMain && culler == gMainCuller && GetCurrentThreadId() == gMainThread) {
		if (gFadeDepth == 0 && SafeCollect(node, true) && gCullingOn && !gFaulted) return;   // proven covered: not drawn
		++gFadeDepth;
		OrigTreeOnVisible(node, culler);
		--gFadeDepth;
		return;
	}
	OrigTreeOnVisible(node, culler);
}

static void __fastcall MultiBoundOnVisibleHook(void* node, void*, void* culler) {
	if (gInMain && culler == gMainCuller && GetCurrentThreadId() == gMainThread && gFadeDepth == 0 &&
		SafeCollectLand(node, culler) && gCullingOn && !gFaulted)
		return;   // proven covered: the quadrant's land is not drawn
	OrigMultiBoundOnVisible(node, culler);
}

static unsigned gOpaqueCalls = 0;

static void __cdecl OpaqueHook(void* camera, void* accumulator) {
	OrigOpaque(camera, accumulator);
	if (!gSet.enabled || gFaulted) return;
	if (++gOpaqueCalls == 600 && gFrame == 1)
		Log("Warning: 600 main opaque passes and none followed the hooked main accumulate (multithreaded rendering on, or "
			"the camera differs): nothing is measured");
	if (!camera || camera != gMainCamera || GetCurrentThreadId() != gMainThread) return;
	LARGE_INTEGER start = {};
	if (kDevTools) QueryPerformanceCounter(&start);

	// Frame time between main views, credited to the state the frame was drawn in (pauses and loads left out).
	static LARGE_INTEGER last = {};
	if (kDevTools && last.QuadPart) {
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
	if (kDevTools && gSet.compareKey && controls && !Game::IsInMenuMode() &&
		Game::GetKeyboardButton(controls, gSet.compareKey, Game::KeyPressed)) {
		if (gStats.frames > 0) LogBlock();   // the numbers so far belong to the old mode
		// One feature at a time against the INI's settings: as set -> land flipped -> grass flipped -> bone throttle
		// flipped -> as set.
		gCompareMode = (gCompareMode + 1) % 4;
		gLandOn = gSet.landCulling != (gCompareMode == 1);
		gGrassOn = gSet.grassCulling != (gCompareMode == 2);
		gAnimThrottleOn = gSet.animThrottle != (gCompareMode == 3);
		gCompareMessage = true;
		Log("[frame %u] compare key: land %s, grass %s, actor bone throttle %s", gFrame, gLandOn ? "ON" : "OFF",
			gGrassOn ? "ON" : "OFF", gAnimThrottleOn ? "ON" : "OFF");
	}
	SafeRunQueries(camera);
	if (kDevTools) gStats.queryUs += MicrosSince(start);
	gCandidates.clear();
	gLampCandidates.clear();
	gMainCamera = nullptr;   // one query pass per main accumulate

	const int slot = gFrame % kRing;
	for (int i = 0; i < 3; ++i) gCamPos[slot][i] = gNowPos[i];
	for (int i = 0; i < 9; ++i) gCamRot[slot][i] = gNowRot[i];
	++gFrame;
	gStats.frames += 1;
	if (gStats.frames >= gSet.logInterval) {
		if (kDevTools) LogBlock();
		else gStats = Stats{};   // release builds keep no numbers
	}
	if (gCompareMessage) {
		gCompareMessage = false;
		static char message[160];
		static const char* const kWhat[4] = { "as set in the INI", "land switched", "grass switched", "bone throttle switched" };
		snprintf(message, sizeof(message), "Occlusion culling (%s): land %s, grass %s, bones %s", kWhat[gCompareMode],
			gLandOn ? "ON" : "OFF", gGrassOn ? "ON" : "OFF", gAnimThrottleOn ? "throttled" : "every frame");
		Game::ShowMessage(message, 0, nullptr, nullptr, 2.0f, true);
	}
	if (gToggleMessage) {
		gToggleMessage = false;
		Game::ShowMessage(gCullingOn ? "Occlusion culling: ON" : "Occlusion culling: OFF (measuring only)", 0, nullptr, nullptr,
			2.0f, true);
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Cell update timing (developer builds, measuring only): the AI thread's TES::UpdateCellAnimations runs the Havok
// step (bhkWorld::Update) and each loaded cell's TESObjectCELL::UpdateManagedNodes, which updates every animated
// reference's scene graph (signs, fans, doors...; trees only inside the view). Animated objects are split like
// actors: drawn / hidden by us / not reached by the main view in the last two frames.

typedef uintptr_t(__thiscall* bhkWorldUpdate_t)(void*);
typedef void(__thiscall* CellUpdateManaged_t)(void*, float, bool);
typedef void(__thiscall* FadeUpdateSelected_t)(void*, void*, uint32_t);

static uintptr_t __fastcall bhkWorldUpdateHook(void* world, void*) {
	LARGE_INTEGER a;
	QueryPerformanceCounter(&a);
	const uintptr_t result = ((bhkWorldUpdate_t)Game::Fn_bhkWorldUpdate)(world);
	const LONG64 t = CellCost::Ticks(a);
	InterlockedAdd64(&CellCost::gPhysTicks, t);
	InterlockedIncrement64(&CellCost::gPhysCalls);
	if (CellCost::tInCell) InterlockedAdd64(&CellCost::gPhysInCellTicks, t);   // an interior's step, inside its cell update
	return result;
}

static void __fastcall CellUpdateManagedHook(void* cell, void*, float time, bool largeOnly) {
	LARGE_INTEGER a;
	QueryPerformanceCounter(&a);
	++CellCost::tInCell;
	((CellUpdateManaged_t)Game::Fn_CellUpdateManagedNodes)(cell, time, largeOnly);
	--CellCost::tInCell;
	InterlockedAdd64(&CellCost::gCellTicks, CellCost::Ticks(a));
	InterlockedIncrement64(&CellCost::gCellCalls);
}

// Only the outermost fade node inside a cell update is timed (the animated reference's root; dropped items under the
// cell's dynamic node count too).
static void __fastcall FadeUpdateSelectedHook(void* node, void*, void* data, uint32_t flags) {
	if (!CellCost::tInCell || CellCost::tInObject) {
		((FadeUpdateSelected_t)Game::Fn_FadeUpdateSelected)(node, data, flags);
		return;
	}
	LARGE_INTEGER a;
	QueryPerformanceCounter(&a);
	++CellCost::tInObject;
	((FadeUpdateSelected_t)Game::Fn_FadeUpdateSelected)(node, data, flags);
	--CellCost::tInObject;
	const LONG64 t = CellCost::Ticks(a);
	const int c = CellCost::Classify(node, gFrame);
	InterlockedAdd64(&CellCost::gObjTicks[c], t);
	InterlockedIncrement64(&CellCost::gObjCalls[c]);
}

typedef void(__thiscall* LockHavok_t)(void*, int);
typedef void(__thiscall* GridUpdate_t)(void*, float);
typedef void(__thiscall* NoArgs_t)(void*);

static void AddPart(CellCost::Part part, const LARGE_INTEGER& a) {
	InterlockedAdd64(&CellCost::gPartTicks[part], CellCost::Ticks(a));
	InterlockedIncrement64(&CellCost::gPartCalls[part]);
}
static void __fastcall LockHavokHook(void* tes, void*, int lock) {
	LARGE_INTEGER a;
	QueryPerformanceCounter(&a);
	((LockHavok_t)CellCost::gPartOrig[CellCost::kLock])(tes, lock);
	AddPart(CellCost::kLock, a);
}
static void __fastcall GridUpdateHook(void* grid, void*, float time) {
	LARGE_INTEGER a;
	QueryPerformanceCounter(&a);
	((GridUpdate_t)CellCost::gPartOrig[CellCost::kGrid])(grid, time);
	AddPart(CellCost::kGrid, a);
}
template <CellCost::Part kPart> static void __fastcall NoArgsHook(void* self, void*) {
	LARGE_INTEGER a;
	QueryPerformanceCounter(&a);
	((NoArgs_t)CellCost::gPartOrig[kPart])(self);
	AddPart(kPart, a);
}

// Only slots and calls that still point at the game's own function are taken.
static void InstallCellTiming() {
	struct { uintptr_t at, target; CellCost::Part part; void* hook; } parts[] = {
		{ Game::Call_LockHavok, 0x00453860, CellCost::kLock, &LockHavokHook },
		{ Game::Call_GridUpdate, 0x004BA9A0, CellCost::kGrid, &GridUpdateHook },
		{ Game::Call_ParallelTasks, 0x0087AA90, CellCost::kTasks, &NoArgsHook<CellCost::kTasks> },
		{ Game::Call_ObjectDamage, 0x00623640, CellCost::kDamage, &NoArgsHook<CellCost::kDamage> },
		{ Game::Call_Projectiles, 0x00975080, CellCost::kProjectiles, &NoArgsHook<CellCost::kProjectiles> },
	};
	int taken = 0;
	for (auto& p : parts)
		if (*(uint8_t*)p.at == 0xE8 && p.at + 5 + *(int32_t*)(p.at + 1) == p.target) {
			CellCost::gPartOrig[p.part] = ReplaceCall(p.at, p.hook);
			++taken;
		}
	Log("Cell update parts timing: %d of 5 calls", taken);
	int cells = 0;
	for (uintptr_t at : Game::Call_CellUpdateManagedNodes)
		if (*(uint8_t*)at == 0xE8 && at + 5 + *(int32_t*)(at + 1) == Game::Fn_CellUpdateManagedNodes) {
			ReplaceCall(at, &CellUpdateManagedHook);
			++cells;
		}
	const bool physics = *(uintptr_t*)Game::Vtbl_bhkWorldUpdate == Game::Fn_bhkWorldUpdate;
	if (physics) ReplaceVirtual(Game::Vtbl_bhkWorldUpdate, &bhkWorldUpdateHook);
	const bool objects = *(uintptr_t*)Game::Vtbl_FadeUpdateSelected == Game::Fn_FadeUpdateSelected;
	if (objects) ReplaceVirtual(Game::Vtbl_FadeUpdateSelected, &FadeUpdateSelectedHook);
	CellCost::gHooked = cells > 0;
	Log("Cell update timing: %d of 2 cell update calls, Havok step %s, animated objects %s", cells, physics ? "hooked" : "NOT hooked",
		objects ? "hooked" : "NOT hooked");
}

// ---------------------------------------------------------------------------------------------------------------
// Actor animation timing (developer builds)

typedef void(__thiscall* ActorUpdateAnim_t)(void*, float);
typedef void(__thiscall* ActorAnimMove_t)(void*);
typedef char(__thiscall* QueuedScenegraph_t)(void*);

typedef void(__thiscall* SetDelayUpdateScenegraph_t)(void*, bool);

static void AddAnimCost(AnimCost::Step step, void* actor, const LARGE_INTEGER& a) {
	LARGE_INTEGER b;
	QueryPerformanceCounter(&b);
	if (!actor || actor == *(void**)Game::PlayerPtr) return;
	const int c = AnimCost::Classify(actor, gFrame);
	InterlockedAdd64(&AnimCost::gSum[step].ticks[c], b.QuadPart - a.QuadPart);
	InterlockedIncrement64(&AnimCost::gSum[step].calls[c]);
}

// Bone transforms of an actor the main view does not draw (hidden by us, or outside the view in the last two frames)
// and that is not near the camera are brought up to date only every iAnimInterval frames. The skipped ones get the
// engine's "update queued" flag cleared, as a done update would, so the next frame queues them again normally.
static bool SkipBones(void* animation) {
	if (!gAnimThrottleOn || !gCullingOn || gSet.animInterval <= 1 || gFaulted) return false;
	void* actor = Game::At<void*>(animation, Game::Anim_Actor);
	if (!actor || actor == *(void**)Game::PlayerPtr) return false;
	const uint32_t now = gFrame;
	if ((now + (uint32_t)((uintptr_t)actor >> 4)) % (uint32_t)gSet.animInterval == 0) return false;   // its turn
	const AnimCost::Class c = AnimCost::Classify(actor, now);
	if (c == AnimCost::kDrawn) return false;
	const float* p = &Game::At<float>(actor, Game::Ref_Position);
	const float dx = p[0] - gNowPos[0], dy = p[1] - gNowPos[1], dz = p[2] - gNowPos[2];
	if (dx * dx + dy * dy + dz * dz < gSet.animNearDistance * gSet.animNearDistance) return false;
	if (kDevTools) InterlockedIncrement64(&AnimCost::gSkipped[c]);
	return true;
}

static void __fastcall ActorUpdateAnimationHook(void* actor, void*, float time) {
	LARGE_INTEGER a;
	QueryPerformanceCounter(&a);
	((ActorUpdateAnim_t)Game::Fn_ActorUpdateAnimation)(actor, time);
	AddAnimCost(AnimCost::kUpdate, actor, a);
}

static void __fastcall ActorUpdateAnimMovementHook(void* actor, void*) {
	LARGE_INTEGER a;
	QueryPerformanceCounter(&a);
	((ActorAnimMove_t)Game::Fn_ActorUpdateAnimMovement)(actor);
	AddAnimCost(AnimCost::kPose, actor, a);
}

static char __fastcall QueuedScenegraphHook(void* animation, void*) {
	if (SkipBones(animation)) {
		((SetDelayUpdateScenegraph_t)Game::Fn_SetDelayUpdateScenegraph)(animation, false);
		return 0;
	}
	LARGE_INTEGER a = {};
	if (kDevTools) QueryPerformanceCounter(&a);
	const char updated = ((QueuedScenegraph_t)Game::Fn_QueuedScenegraph)(animation);
	if (kDevTools && updated) AddAnimCost(AnimCost::kBones, Game::At<void*>(animation, Game::Anim_Actor), a);
	return updated;
}

// Only slots and calls that still point at the game's own function are taken (another plugin's hook is left alone).
static void InstallAnimHooks() {
	int updates = 0, moves = 0, bones = 0;
	if (!kDevTools && !(gSet.animThrottle && gSet.animInterval > 1)) return;
	if (*(uint8_t*)(Game::Fn_SetDelayUpdateScenegraph) != 0x55) {   // push ebp: the function is where it should be
		Log("Actor bones: 0x%08X is not Animation::SetDelayUpdateScenegraph; actor animation left alone",
			(unsigned)Game::Fn_SetDelayUpdateScenegraph);
		return;
	}
	for (uintptr_t slot : Game::Vtbl_ActorUpdateAnimation)
		if (kDevTools && *(uintptr_t*)slot == Game::Fn_ActorUpdateAnimation) { ReplaceVirtual(slot, &ActorUpdateAnimationHook); ++updates; }
	for (uintptr_t slot : Game::Vtbl_ActorUpdateAnimMovement)
		if (kDevTools && *(uintptr_t*)slot == Game::Fn_ActorUpdateAnimMovement) {
			ReplaceVirtual(slot, &ActorUpdateAnimMovementHook);
			++moves;
		}
	for (uintptr_t at : Game::Call_QueuedScenegraph)
		if (*(uint8_t*)at == 0xE8 && at + 5 + *(int32_t*)(at + 1) == Game::Fn_QueuedScenegraph) {
			ReplaceCall(at, &QueuedScenegraphHook);
			++bones;
		}
	AnimCost::gHooked[AnimCost::kUpdate] = updates > 0;
	AnimCost::gHooked[AnimCost::kPose] = moves > 0;
	AnimCost::gHooked[AnimCost::kBones] = bones > 0;
	Log("Actor animation hooks: %d of 4 UpdateAnimation slots, %d of 4 UpdateAnimMovement slots (timing, developer builds), %d "
		"of 3 bone transform calls", updates, moves, bones);
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
	OrigTreeOnVisible = (OnVisible_t)ReplaceVirtual(Game::Vtbl_TreeOnVisible, &TreeOnVisibleHook);
	// lStewieAl's Tweaks replaces this slot with its own copy of the game's function (Rendering::InitHooks), so the
	// hook chains to whatever is there.
	OrigMultiBoundOnVisible = (OnVisible_t)ReplaceVirtual(Game::Vtbl_MultiBoundOnVisible, &MultiBoundOnVisibleHook);
	Log("Land hook installed: BSMultiBoundNode::OnVisible -> 0x%08X%s", (unsigned)(uintptr_t)OrigMultiBoundOnVisible,
		(uintptr_t)OrigMultiBoundOnVisible != Game::Fn_MultiBoundOnVisible ? " (through another plugin's hook, e.g. Stewie's Tweaks)" : "");
	Log("Tree hook installed: BSTreeNode::OnVisible -> 0x%08X%s", (unsigned)(uintptr_t)OrigTreeOnVisible,
		(uintptr_t)OrigTreeOnVisible != 0x0066BF50 ? " (through another plugin's hook)" : "");
	OrigLampTest = (LampTest_t)ReplaceCall(Game::Call_LampTest, &LampTestHook);
	InstallAnimHooks();
	if (kDevTools) InstallCellTiming();
	Log(OrigLampTest ? "Lamp test hook installed -> 0x%08X%s" : "0x%08X: the lamp test call is not a call; lamps are left alone%s",
		OrigLampTest ? (unsigned)(uintptr_t)OrigLampTest : (unsigned)Game::Call_LampTest,
		OrigLampTest && (uintptr_t)OrigLampTest != 0x00B9E970 ? " (through another plugin's hook, e.g. NVR's KeepLampsInView)" : "");
	Log("Hooks installed: main accumulate -> 0x%08X, main opaque pass -> 0x%08X, BSFadeNode::OnVisible -> 0x%08X",
		(unsigned)(uintptr_t)OrigAccumulate, (unsigned)(uintptr_t)OrigOpaque, (unsigned)(uintptr_t)OrigFadeOnVisible);
	gCullingOn = gSet.culling;
	gOrientedOn = gSet.orientedBoxes;
	gActorsOn = gSet.actorCulling;
	gTreesOn = gSet.treeCulling;
	gActivatorsOn = gSet.activatorCulling;
	gRevealOn = gSet.revealDepth > 0.0f;
	gAnimThrottleOn = gSet.animThrottle;
	gLandOn = gSet.landCulling;
	gGrassOn = gSet.grassCulling;
	if (kDevTools)
		Log("Hiding starts %s; key 0x%02X switches it. A block of numbers is logged every %d frames of world view.",
			gCullingOn ? "ON" : "OFF (measuring only)", gSet.toggleKey, gSet.logInterval);
	else
		Log("Hiding starts %s; key 0x%02X switches it.", gCullingOn ? "ON" : "OFF", gSet.toggleKey);
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
	Log("Occlusion Culling %s, %s (objects, actors, doors/activators, SpeedTree trees and hidden lamps)", kVersion, OC_BUILD);
	QueryPerformanceFrequency(&gQpcFreq);
	LoadSettings();
	auto* messaging = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
	if (!messaging || !messaging->RegisterListener(nvse->GetPluginHandle(), "NVSE", MessageHandler))
		Log("No NVSE messaging: disabled");
	return true;
}

BOOL WINAPI DllMain(HANDLE, DWORD, LPVOID) { return TRUE; }
