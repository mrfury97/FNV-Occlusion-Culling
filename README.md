# FNV Occlusion Culling

An xNVSE plugin for Fallout: New Vegas (1.4.0.525) that stops the game from drawing large objects the graphics card has proven are completely hidden behind other things. It works on its own and alongside New Vegas Reloaded. It doesn't change any game logic: AI, scripts, physics and collision behave exactly as before.

## Results

Measured in game with the hotkey switching hiding on and off in the same spot (NVR P83 playtest, DXVK, MSAA 4x):

| Area | Hiding off | Hiding on | Change |
|---|---|---|---|
| Freeside (Bus North, Old Mormon Fort) | 30 fps | 46 fps | +57% |
| Vault 21 corridors | 48 fps | 82 fps | +71% |
| Vault 22 Common Areas | 45 fps | 54 fps | +20% |
| House Resort (interior) | 92 fps | 106 fps | +15% |
| Open ground (Strip avenues, Camp Golf) | | | about even |

The gain is largest where the CPU is busy sending draw calls for things you can't see: dense towns behind walls, and lamp-lit interiors where every object is drawn several times. It barely changes open ground, where little is hidden and most of the cost is terrain, shadows and screen effects.

## How it works

- **Finding the objects.** While the game gathers the main view (`TESMain::DrawScene` 0x873200, its `BSShaderUtil::AccumulateScene` call at 0x87415B), each placed object's root `BSFadeNode` is checked in `OnVisible` (vtable slot 53). Large objects become candidates.
- **Never touched:** actors, doors, lights, activators, terminals, projectiles and anything near the camera. Small objects and distant LOD are optional (see the INI).
- **Asking the GPU.** Right after the opaque pass (the `RenderScenePreResolveDepth` call at 0x874180), the depth buffer holds the finished scene. Each candidate's box (the union of its shapes' bounds) is drawn there inside a D3D9 occlusion query, with colour and depth writes off.
  - **Projection:** the boxes are projected on the CPU with the renderer's own camera-relative view and projection, and drawn as pre-transformed vertices. Boxes reaching behind the camera aren't tested.
  - **Depth direction:** detected automatically (NVR uses reversed depth).
  - **Answers:** read a frame later without waiting.
- **Hiding.** An object is left out of the main view only when its last two answers drew zero pixels, the newest is at most 4 frames old, and the camera hasn't turned or moved too far since. Shadows, reflections and every other pass still draw it.
- **Scheduling.** Covered objects are retested every frame so they reappear promptly. Visible objects are retested every few frames. The per-frame budget goes to whoever has waited longest (`src/Scheduler.h`).
- **Safety.**
  - Only a few device states are changed: colour writes, depth write/test, culling, scissor/stencil when on, shaders, vertex format and stream 0. Each is read first and set back to exactly that value.
  - No state blocks are used. Re-sending states (a full snapshot, or toggling alpha testing) disturbs NVIDIA's alpha-to-coverage driver hack, which the game uses with `bTransparencyMultisampling` and MSAA. That left a dithered speckle on most surfaces until restart. Alpha testing is never touched: if it's on at that point, nothing is tested that frame.
  - Hooks chain to anything already there.
  - Any exception switches the plugin off for the session.
  - A device reset drops all queries.

## Install

1. Build `OcclusionCulling.vcxproj` (Visual Studio 2019 build tools, v142, Release | Win32), or take the DLL from a release.
2. Put `OcclusionCulling.dll` and `OcclusionCulling.ini` in `Data\NVSE\Plugins\`.
3. The log goes to `logs\OcclusionCulling.log` in the game folder. Every 600 frames it records:
   - what was tested and hidden;
   - the plugin's own cost;
   - the average frame time with hiding on vs off.

## Settings (`OcclusionCulling.ini`)

| Key | Default | Meaning |
|---|---|---|
| `bCulling` | 1 | Hiding on at start |
| `iToggleKey` | 210 (Insert) | Switches hiding on and off in game; a corner message shows the state |
| `fMinRadius` | 150 | Smaller objects are never tested (75 also covers more clutter) |
| `fNearDistance` | 400 | Objects closer than this are never tested |
| `bDistantLOD` | 0 | Also test distant LOD objects |
| `iMaxQueriesPerFrame` | 500 | Test budget per frame (1000 helps very dense areas) |
| `iMaxAnswerAge` | 4 | Oldest answer, in frames, that may hide an object |
| `iVisibleRetestFrames` | 4 | How often visible objects are retested |
| `fGuardDegreesPerFrame` / `fGuardUnitsPerFrame` | 2.5 / 32 | Camera turn/move per frame of answer age beyond which answers aren't used |
| `bDebugBoxes` | 0 | Draws the tested boxes (green visible, red hidden) |
| `bActorCulling` / `fActorMargin` | 1 / 24 | NPCs and creatures (not the player) are hidden when proven covered, only while still inside the box that was tested (bound + 10% + margin). Animation, AI and shadows carry on as when they are outside the view |
| `bActivatorCulling` / `fActivatorMargin` | 1 / 16 | Doors, activators, talking activators and terminals are hidden when proven covered; like actors, their box follows them every frame (bound + 5% + margin) and they stay hidden only while inside the box that was tested |
| `bTreeCulling` / `fTreeMargin` | 1 / 32 | SpeedTree trees and shrubs (their own node type, which the object hook never saw) are hidden when proven covered; their box is the bound plus 20% + margin, since leaf cards are spread out on the graphics card |
| `bOrientedBoxes` | 1 | Tighter boxes: each shape's own vertices give its extent, and the box follows the object's rotation. Shapes whose vertices the game has freed keep their bound spheres; animated objects keep the old box |
| `iCompareKey` | 207 (End) | Switches one feature at a time against your settings (as set, actors switched, oriented boxes switched, trees switched, doors/activators switched); the log starts a new block at each press |

## Tests

`tests\build_test.bat` runs the maths checks (including oriented boxes). `tests\build_sched.bat` runs the scheduler simulation (fort, open street, camera turning, over budget).

## Credits

- The approach was inspired by [Faster Objects](https://github.com/1001Bits/Faster-Objects) (Skyrim, GPL-3), whose Hi-Z culling showed what main-view-only culling can do. No code was taken from it.
- The camera guard and the object identity check follow ideas from New Vegas Reloaded's `HiddenObjectCulling` (NVR Unofficial Optimized, GPL-3).
- `nvse/` holds a minimal subset of the [xNVSE](https://github.com/xNVSE/NVSE) plugin API headers.

## Licence

GPL-3.0; see `LICENSE`.
