# GTA III VR on the Steam Frame: stage 1 plan

This is the working plan for the first playable stage. It reuses the Vice City
VR port (`~/code/vicecity-vr-work/tree`, "V") on top of re3 (this repo, "G").
Pristine reVC is at `~/code/vicecity-vr-work/reVC` ("R"), and the Vice City kit
is at `~/code/vicecity-vr` ("K"). It was written on 2026-10-03 from a read-only
survey of all four. Check line numbers against the files before relying on
them.

## What stage 1 is

GTA III as an Android APK in Lepton, with:

- head-tracked stereo in first person, on foot and in vehicles;
- the HUD on the Im2D plane;
- frontend, menus and loading on the theater screen, and cutscenes on the 3D
  cinema screen;
- the Frame system layer: Menu is Start, View short is back or toggle view,
  View hold is recentre;
- both third-person modes;
- gamepad-style weapons and driving;
- data in `/sdcard/Documents/GTA3VR`;
- the `vr-diag.ini` diagnostics, including `diag_new_game`;
- the Frame build and deploy flow.

Not in stage 1:

- hand-tracked weapons (stage 2, behind `GTA_VR_WEAPONS`);
- physical steering (stage 3);
- ragdolls, deformation, model sets, the physics director, and the VR
  cheat/mission menus.

## Decisions (recommended defaults, all settings)

| # | Decision | Default |
| --- | --- | --- |
| D1 | View at first launch | THIRD: 3D SCREEN (the user found first person intense); View short toggles |
| D2 | World scale | 1.0 GTA unit = 1 m, as in Vice City |
| D3 | First-person on-foot aim | Head-aim: the ped turns to the head's yaw while firing; movement HEAD |
| D4 | Sniper, M16 and rocket camera modes | The headset drives the camera's Beta/Alpha in stereo, no zoom |
| D5 | In-car view | Yaw only; full car basis behind a `VehicleFollowRotation` setting |
| D6 | Macro names | Keep `MIAMIVR_*`, so fixes cherry-pick between the ports |
| D7 | Variants | Frame only, no Quest build |
| D8 | Island loading | Force `ISLAND_LOADING_HIGH` |
| D9 | Vice City colour filter | Off |
| D10 | Names | `com.gta3vr.frame`, Steam title `GTA3VR`, Steam app 12100 |
| D11 | `-fsigned-char` | Yes |

## Findings that shape the work

- The Steam Frame layer in V touches no engine files. Third person, 3D cinema,
  the system layer and the diagnostics are all application-layer code (`android/`,
  `src/skel/android`, `third_party/sfvr`).
- G's `vendor/librw` is identical to V's. Stereo is one multiview pass, and
  librw builds `worldToPlay` from `setFirstPersonAnchor`. There is no
  engine-side per-eye loop.
- Most of V's big engine diff is either tracked weapons or the desktop D3D12
  path (`GTA_VR_OCULUS`, `RW_D3D12`, never defined on Android). The
  `GameRender::Submit*` changes are pass-throughs. Port about 20 re3 files by
  intent rather than applying the 21 kit patches.
- re3 and reVC share the platform skeleton (`platform.h`, `crossplatform.h`),
  the `CPad::UpdatePads → CapturePad(0)` seam, `CControllerState`,
  `CCutsceneMgr::IsRunning/IsCutsceneProcessing`, and
  `TheCamera.m_WideScreenOn/Cams[].Front/Beta/m_fTrueBeta/m_fTargetBeta`.
- re3 has a POSIX CD stream at `src/core/CdStream_posix.cpp`. The kit's
  `CdStreamPosix` hunks map onto it.

## Layout

Work directly in G, on branch `steam-frame`. Guards:

- `#if defined(RW_VULKAN) && defined(__ANDROID__)` for engine hooks;
- `MIAMIVR_STEAM_FRAME` for Frame code;
- `MIAMIVR_DEV_TOOLS` stays, because librw tests it;
- `GTA_VR_WEAPONS` is not defined in stage 1.

Copy verbatim from V:

- `third_party/sfvr/`;
- `android/{gradlew, gradle/, build.gradle.kts, gradle.properties}`;
- `android/app/src/main/cpp/{xr_vulkan_session.*, frame_diag.*, platform_android.*, shaders/}`;
- `src/skel/android/VrVehicleSeatRecenter.h`.

Copy and adapt:

- `settings.gradle.kts`: set `rootProject.name`.
- `app/build.gradle.kts`:
  - namespace and id `com.gta3vr`, with `.frame`;
  - only the `frame` build type, with `minSdk 30`;
  - no Quest release signing and no `SaveProvider`.
- Manifests: `src/main`, plus `src/frame` (head tracking not required, `MANAGE_EXTERNAL_STORAGE`).
- `android_main.cpp`:
  - storage root `/sdcard/Documents/GTA3VR`, plus the app strings;
  - wrist-panel code behind `GTA3VR_WRIST_PANELS` (= 0);
  - colour filter off by default.
- `src/skel/android/{android.cpp, android.h}` and `vrdebug.cpp` (forked to
  `vrmenu.cpp`, see "Carving").
- `src/audio/sampman_android.cpp`:
  - re3's `sampman.h` uses `bool8` returns and `MAX_STREAMS` 2;
  - it has no `SetMP3BoostVolume`.
- `K/tools/steam_frame/{build.sh, deploy.sh, tests/check_bindings.py}`:
  - `build.sh` just runs `./gradlew :app:assembleFrame` in `G/android`;
  - `deploy.sh` uses title `GTA3VR`, data `Documents/GTA3VR`, folders
    `data TEXT anim txd models audio mp3 skins movies`, `--from-steam` app
    12100, and overlays `G/gamefiles`.

Do not copy:

- `QuestProfiler.cpp`: replace it with no-op stubs for the 18 functions in
  `QuestProfiler.h`.
- `vrhands_quest.cpp`, `VrCheatMenu.h`, `src/vr/*`, `ModelSets`,
  `VrRagdoll*`, `PhysicsDirector`, `VehicleDeformation`,
  `VehicleVisualDirector`, `GameRenderCommand`, `VrCullViz`.

CMake (from V):

- `REVC_ROOT` → `RE3_ROOT`, and the library is `gta3vr`.
- `GAME_DIRS` uses `renderer` (not `render`) and adds `skel/android`.
- Drop the explicit Quest-only source lists.
- Keep:
  - the `sampman_oal`/`sampman_miles` removals;
  - GitSHA1;
  - the librw, shader and sfvr blocks;
  - `-u ANativeActivity_onCreate`;
  - `MIAMIVR_BRINGUP`.
- Definitions `LIBRW RW_VULKAN AUDIO_ANDROID`, and `-fsigned-char`.

## Engine integration (re3 files)

Legend: AS-IS means the kit hunk text applies; HAND means port by intent.

### Build portability

- `config.h` (HAND):
  - `EXTENDED_COLOURFILTER/EXTENDED_PIPELINES/SCREEN_DROPLETS/NEW_RENDERER` go behind `#ifndef RW_VULKAN`;
  - raise `NUMVISIBLEENTITIES`/`NUMALPHAENTITYLIST` under `RW_VULKAN`.
- `common.h` (AS-IS): `<algorithm>` before `clamp`, plus `RE3_CHECKPOINT`.
- `crossplatform.h/.cpp` (AS-IS): the `CapturePad` declaration, and a null-`opendir` guard in `casepath`.
- `re3.cpp`: include `crossplatform` on non-Win32.
- `fakerw/fake.cpp`: nil-raster guard.
- `rw/TxdStore.cpp`: a bounded open retry.
- `audio/sampman_null.cpp`: add `!AUDIO_ANDROID`.

### CdStream (`src/core/CdStream_posix.cpp`, HAND)

- `USE_UNNAMED_SEM` on Android;
- no `O_NOATIME`;
- names are `[1024]`;
- replace `assert(false)` with a diagnostic.

### Render loop (`src/core/main.cpp`, HAND)

- `DoRWStuffStartOfFrame_Horizon`:
  - `VrApplyEyeFov()` before `CameraSize`;
  - `VrCaptureViewWindow()` and `VrUpdateFirstPersonAnchor(true)` before `RsCameraBeginUpdate`;
  - clear colour instead of `CClouds::RenderBackground`.
- Skip `DoRWRenderHorizon` on Vulkan.
- In `Idle`:
  - `CRenderer::SetVrPerViewCulling` around `RenderScene`;
  - no `RenderMotionBlur`.
- `Render2dStuff`:
  - Im2D safe-area transform, full screen in theater;
  - widescreen borders only when `VrCutsceneMode()==0`.

### Timers

No change. `psTimer` in `android.cpp` runs on the predicted display time.

### HUD and 2D

- `renderer/Draw.cpp`: `CalculateAspectRatio` returns `VrTheaterAspectRatio()` in theater.
- `renderer/Sprite2d`: `maVertices[40]`.
- `core/Radar.cpp`: Vice City's 24-gon tile clipping in place of the depth mask, which fails under Adreno multiview.

### Coronas, sky, water

- `renderer/Coronas.cpp`:
  - the theater remap;
  - no streaks;
  - reflection `recipz`;
  - HEADLIGHT/STREAK → NORMAL.
- `renderer/WaterLevel.cpp:613-624`: `WaterViewForward()` instead of `TheCamera.GetForward()`.
- `Clouds::Render` returns early on Vulkan (port the world-space sky later).

### Culling (`renderer/Renderer.{h,cpp}`, HAND)

- `IsEntityOutsideCurrentView` at every `GetIsOnScreen*` test.
- `ShouldRenderInCurrentView`, with only the OFF/SAFE branches.
- In `RenderOneNonRoad`, hide the player when
  `gVrHidePlayerBody && gVrFirstPersonActive`.
- Bounds-checked `InsertEntityIntoList`.
- Omnidirectional `ScanWorld`. Keep re3's subway branch
  (`ScanSectorList_Subway`), and use big-building lists
  `LEVEL_INDUSTRIAL/COMMERCIAL/SUBURBAN`.
- re3 has no occlusion system, so skip all of that.

### Cutscenes

- The theater predicate and 3D cinema functions in `android.cpp` compile
  as-is. `CGame::playingIntro` is never set in GTA III.
- `objects/CutsceneObject.cpp`: Vice City's black-actor lighting fix.

### Streaming

- Force `ISLAND_LOADING_HIGH` after the ini load.
- Port Vice City's guard around the blocking
  `CStreaming::LoadAllRequestedModels(true)` on island change
  (`collision/Collision.cpp:271`).
- `PlayerInfo.cpp`: null guard in `FindPlayerCoors`.

### Frontend

Keep `PC_MENU` (`TRIANGLE_BACK_BUTTON`). Deploy re3's `gamefiles` over the
retail data, or the custom option labels are missing.

## Carving the Vice City Android code

### `android.cpp`

Compile it without `GTA_VR_WEAPONS`, then:

- `thirdPersonVehicle = VrThirdPersonShoulder() && player->InVehicle() && !vehicleTransition`;
- `gVrHidePlayerBody` uses the weapons-branch expression without
  `IsImmersiveDrivingActive`. Otherwise Claude stays hidden in over-shoulder mode.
- Remove the bike seat block.
- Ungate the vehicle-basis block behind `VehicleFollowRotation`, without the
  bike branches.
- `CStreaming::RemoveLeastUsedModel()` takes no arguments in re3.
- `appName` "GTA III VR".

In `CapturePad`, with no tracked weapons, every on-foot mode uses the pad
layout:

- RT → Circle (fire), LT → R1 (target);
- bumpers → L2/R2;
- `VrApplyPadBindings(..., false)`.

### `vrdebug.cpp`

Fork it into `src/skel/android/vrmenu.cpp`.

Keep:

- glyphs and pixels, `LoadVrSettings` and the `Save*` helpers;
- the root page with GRAPHICS, HUD, LOCOMOTION (all items, including VIEW and
  CUTSCENES), CONTROLS, ABOUT, and Quit to Steam;
- every `androidgame::Vr*` accessor in `android.h`.

Delete the LIGHTING, WATER, WEAPONS, WRIST_RADAR, TRAFFIC, RAGDOLL,
DEFORMATION, MODEL_ASSETS, VEHICLE(_CALIBRATION), CALIBRATION, HOLSTERS,
CHEATS and MISSIONS pages, together with their handlers and drawers. Also
delete the `OculusVR::*` calls.

### `QuestProfiler`

Stub it.

## GTA III differences

- There are no motorbikes.
- GTA III adds boats, the Dodo, RC cars, the Rhino turret and the subway.
- There is no camera or laser-scope weapon. The first-person weapon camera
  modes are `MODE_SNIPER`, `MODE_M16_1STPERSON` and `MODE_ROCKETLAUNCHER`,
  with their `*_RUNABOUT` variants.
- Pad play is lock-on based.
- Cutscene heads are separate `CCutsceneHead` objects, and
  `GetNumCutsceneObjects` is missing.
- `CControllerState`, the pad mode and the default button roles match Vice
  City.

## Ordered tasks

| # | Task | Acceptance |
| --- | --- | --- |
| T0 | `docs/VR_ARCHITECTURE.md` (probe facts, draw inventory, frame protocol, status table, decisions) | Committed |
| T1 | Android scaffold with `MIAMIVR_BRINGUP=ON`, sfvr, Frame manifest, ids | `assembleFrame` builds on the Mac; `aapt` shows `com.gta3vr.frame` and minSdk 30; bindings test passes |
| T2 | `tools/steam_frame/{build.sh, deploy.sh}` for GTA3VR (`--from-steam` 12100, gamefiles overlay, `--register-title`, `--diag`, `--setting K=V`) | `build.sh` produces the APK; data lands in `~/Documents/GTA3VR/gamedata` |
| T3 | Full game target links: CMake, portability edits, `android.cpp` carve, profiler stub, `vrmenu` fork, `sampman_android` | `libgta3vr.so` links; no `OculusVR`/ModelSets symbols; only guarded regions changed |
| T4 | Boot to the frontend on the device (`force_render`, `dump_frame`) | Frontend reached; `[vr-perf]`; no crash for 3 min; eye dumps show the menu on the theater screen |
| T5 | Stereo world (`main.cpp`, `Draw`, `Renderer`, `Coronas`, `Radar`, `Water`, `Clouds`, island-load and `PlayerInfo` guards) | `diag_new_game` dumps at yaw 0/90/180 differ between eyes; no sea holes; HUD on the plane; radar round |
| T6 | Cutscenes and loading (CINEMA 3D, letterbox, actor lighting) | Cutscene dump shows two different eye images on the screen |
| T7 | Gameplay modes (pad mapping, View toggle, recentre, third person incl. vehicles, hide-body fix, head-relative movement, head aim, `VehicleFollowRotation`) | Bindings test; dumps with `ViewMode=1/2`; mode-switch logs |
| T8 | Audio on the device | SFX banks and streams load |
| T9 | Unattended acceptance from a clean build | Stereo dumps (first person, both third-person modes, cutscene); status table updated |

Then comes the first wear: menus, intro cutscene, walking, driving, both
third-person modes, pause and recentre.

## Risks

- The `vrmenu` carve has hidden couplings.
- The blocking island-change load needs its guard.
- First-person pad aiming is new design.
- Radar and cutscene-actor rendering need Vice City's fixes up front.
- Kit hunk contexts don't match, so port by intent.
- Steam-version audio formats are unchecked.
- The headset is shared, so check for other running Lepton apps before any
  unattended run (playbook lesson 46).
