# GTA III VR on the Steam Frame: architecture (current state)

This is re3 (GTA III) with the Vulkan multiview librw fork and the
Android/OpenXR layer from the Vice City VR port, retargeted at the Valve Steam
Frame. How to build, deploy and test is in [STEAM_FRAME.md](STEAM_FRAME.md). The
plan this follows, with the reasons for each choice, is
[PORT_PLAN.md](PORT_PLAN.md). Places where something is stubbed are listed in
[STUBS.md](STUBS.md).

Status in one line: the port boots, starts a game and renders in stereo with
nobody wearing it. Nobody has played it yet. Section 9 says which claim is
backed by what.

## 1. Target and probe facts

The Frame facts are the ones the Vice City kit probed on 2026-10-03. They are
properties of the headset, so they carry over unchanged.

| Item | Value | Source |
| --- | --- | --- |
| Package route | Android APK in Lepton (compat tool `lepton`), devkit title `GTA3VR` | as the Vice City port, GoldenEye VR and Halo CE VR |
| Application id | `com.gta3vr.frame` (`aapt dump badging`), label "GTA III VR", minSdk 30, arm64-v8a only | host build |
| Steam app for the game data | 12100 (Grand Theft Auto III) | PORT_PLAN D10 |
| Container | Android 11, API 30, arm64-v8a | `getprop ro.build.version.sdk` = 30 |
| Android features | `vulkan.level=1`, `vulkan.version=0x401000` (1.1), `vulkan.compute`; **no** `android.hardware.vr.headtracking` | `pm list features` |
| OpenXR runtime | SteamVR OpenXR, `/vendor/etc/openxr/1/active_runtime.json`; no loader broker | `ls` |
| Loader | Khronos `openxr_loader_for_android` 1.1.43 | `build.gradle.kts` |
| Graphics binding | `XR_KHR_vulkan_enable2`; the runtime creates the Vulkan instance and device | runs on the Frame (unattended, T4) |
| Swapchain | one image, `arraySize = 2`, multiview; an sRGB image is created mutable-format and rendered through a UNORM view | as the Vice City port |
| API version | OpenXR 1.0 requested (1.1 retried if refused) | every Frame-proven port asks for 1.0 |
| Data root | `/sdcard/Documents/GTA3VR` in Lepton, `~/Documents/GTA3VR` on the headset | `android_main.cpp` |
| Native library | `libgta3vr.so`, one target holding the game, librw (Vulkan) and the app layer | `CMakeLists.txt` |

The headtracking feature is why the Frame manifest exists: a manifest that
requires it would not install in Lepton. This port has no Quest variant (D7).

## 2. Build flag and guards

| Guard | Meaning |
| --- | --- |
| `MIAMIVR_STEAM_FRAME` | Frame-only code. CMake option, set by the Gradle build type `frame`, defined 0 or 1 for every target through librw's public definitions. The names keep the Vice City prefix so fixes cherry-pick between the ports (D6) |
| `RW_VULKAN && __ANDROID__` | Every engine (re3) change. The desktop builds compile as before |
| `MIAMIVR_DEV_TOOLS` | Developer diagnostics. Off unless `-PmiamivrDevTools=true`. librw tests it, so it stays |
| `MIAMIVR_BRINGUP` | The OpenXR/Vulkan bring-up target with no game. Off in the Frame build |
| `GTA_VR_WEAPONS` | Tracked hands and physical weapons. Never defined here (stage 2) |
| `GTA3VR_WRIST_PANELS` | Wrist panels. 0 |

Compile definitions on the game target: `LIBRW RW_VULKAN AUDIO_ANDROID`,
plus `-fsigned-char` (D11). The Frame build runs `./gradlew :app:assembleFrame`
in `android/`, or `tools/steam_frame/build.sh`.

## 3. Frame protocol

- A game thread owns the session. It runs `xrWaitFrame`, `xrBeginFrame`,
  `syncInput`, `xrLocateViews` and then the renderer callback.
  `renderGameFrame` steps the game once per XR frame and draws both eyes in one
  multiview pass. Then it calls `xrEndFrame`.
- Game time comes from `predictedDisplayTime` (`psTimer`). While frames flow it
  advances in exact display intervals. When they stop (a blocking load, a paused
  session) it falls back to the wall clock after about 50 ms, so anything that
  waits for time cannot hang.
- When nothing renders, the sim does not step (`shouldRender=false`, standby).
  `force_render` exists for that reason (section 8).
- Reference space is STAGE, falling back to LOCAL. A VIEW space carries the
  head-locked menu quad.
- The pad is injected at one seam: `CPad::UpdatePads` calls `CapturePad(0)` in
  `android.cpp`, which also updates the first-person anchor.

## 4. Draw inventory

The engine side comes from PORT_PLAN's engine section. "Done" means built and
seen working in the unattended run (section 9); "carried" means the code is in
and builds, but nothing has exercised it.

| Draw | VR rule in this port | State |
| --- | --- | --- |
| World | One multiview pass; per-eye view-projection from the runtime FOV. librw builds `worldToPlay` from `setFirstPersonAnchor`. `DoRWStuffStartOfFrame_Horizon` applies the eye FOV and anchor | done |
| Culling | Per-view culling (`IsEntityOutsideCurrentView`, `ShouldRenderInCurrentView`), omnidirectional `ScanWorld`, bounds-checked entity lists. re3 has no occlusion system, so none of that is carried. `VR_CULLING_OFF` exists but nothing sets it | done |
| Player body | Hidden on foot in first person (`gVrHidePlayerBody`); shown in both third-person views, in cars, and in scripted scenes | done on foot; the rest carried |
| Driver head in first person | `CPed::PreRender` collapses the occupant's head while the eye sits inside it | carried (T7) |
| HUD (Im2D) | Projected per eye onto a plane inside the multiview pass; safe-area transform; always drawn | done |
| Radar | Vice City's 24-gon tile clipping in place of the depth mask, which fails under Adreno multiview | done |
| Coronas | Compact, no streaks, reflection `recipz`, headlight/streak types folded to normal | done |
| Water | `WaterViewForward()` (the headset's forward, not the character's) | done |
| Sky and clouds | `CClouds::Render` returns at once on Vulkan, so the sky is the clear colour. No clouds, sun glow or rainbow | **not done** |
| Frontend menus, loading | Theater mode: a 2.56 x 1.44 m quad layer, yaw-anchored at 2 m, 16:9 | done |
| Cutscenes, CINEMA 3D | The screen is submitted as a projection layer. Each eye has the screen's orientation and a FOV that exactly fits its rectangle. The camera is split into two parallel off-axis cameras converged 1.5 m ahead, depth 0.35 / 0.55 / 0.75 for SUBTLE / default / DEEP. Applies to formal cutscenes and to scripted widescreen scenes (GTA III holds `playingIntro` through the whole opening, so the predicate lets camera shots through and keeps the gaps flat) | done |
| 3D SCREEN gameplay | The stock camera on the theater screen with the cutscene setting's 3D depth (`VrScreenGameplayActive`) | carried (T7) |
| Stereo cutscenes | Staged cameras selectable per scene (R3 cycles, L3 keeps). `VrCutsceneCameraActor` is not read by `CCutsceneObject` yet, so the camera actor's head is not collapsed | carried; T6 left open |
| VR menu and debug overlay | 1024x768 head-locked quad layer in VIEW space | carried (the menu is the Vice City one, cut down; see STUBS) |
| Dynamic lights, car reflections | An empty light list is handed to the backend | stubbed |
| Windscreen alpha pass | Not ported. Look at it if cars read as translucent | not done |
| Wrist panels | Off (`GTA3VR_WRIST_PANELS` 0) | not in stage 1 |

Streaming: island loading is forced to `ISLAND_LOADING_HIGH` after the ini load,
the blocking `LoadAllRequestedModels(true)` on an island change is guarded, and
the per-card TXD.IMG cache is never built on Vulkan.

## 5. Input

The Frame controller is a split gamepad. Bindings are in
`xr_vulkan_session.cpp` (Frame, Index and Touch tables) and are checked on the
host by `tools/steam_frame/tests/check_bindings.py`.

| Physical | Action |
| --- | --- |
| R Menu | Start (pause) |
| L View, short press | Back in the VR menu (B) and in the frontend (a Triangle pulse); during play, switch first and third person |
| L View, hold 1 s or 3 s | Recentre, with the system buzz on both hands. There is no standing-height calibration, so 3 s recentres again. A recentre from the runtime does the same |
| L D-pad | The game's D-pad, in the frontend and in play |
| Both grips + Menu | VR menu |
| Left stick | Move. In the frontend, the D-pad with hysteresis |
| Right stick | Turn (smooth or snap) and look up and down. On the 3D screen it drives the stock camera raw |
| A B X Y | Cross, Circle, Square, Triangle by default; on foot they follow the CONTROLS page |
| Grips | L1 and R1 by default; on foot they follow the CONTROLS page |
| Stick clicks | L3 and R3 by default. R3 (look behind) is off on foot in first person unless CONTROLS turns it on |

On foot the triggers are a pad's own layout: **right trigger fires** (Circle),
**left trigger targets** (R1), and the **bumpers cycle weapons** (L2 and R2).
In a vehicle they keep the Vice City mapping: right trigger is the accelerator
(Cross), left trigger is brake and reverse (Square), and the bumpers do
nothing. Remote-controlled vehicles count as vehicles, with A held back from
its own binding so the stock fire action is not pressed twice.

Index fallback: left B is Menu and left A is View. X and Y are reachable only
through remapping.

## 6. Views and gameplay modes

`ViewMode` picks the view; View's short press swaps first person with the last
third-person style.

| View | What it is |
| --- | --- |
| THIRD: 3D SCREEN (default, D1) | Everything on foot, in cars, boats and RC vehicles plays on the theater screen from the stock camera. The right stick drives the camera raw; no head turning, no snap or smooth scaling. A recentre re-places the screen |
| THIRD: OVER SHOULDER | Stereo from the stock chase camera. On foot via `thirdPersonOnFoot`; in vehicles via `thirdPersonVehicle` (the style is on, the player is in a vehicle, and no enter or exit is under way). The view is the chase camera's position and heading through the first-person machinery, so the horizon stays level. Entering or leaving a vehicle holds the last chase view still (`gVrChaseHold*`) |
| FIRST PERSON | The anchor is the player's head bone, nudged 0.12 m forward, with a level horizon. In a vehicle it is yaw-only unless `VehicleFollowRotation` is on, when the whole car basis is followed |

Third person shows the body and the full HUD, and the stick is not turned by
the head. First person hides the body on foot.

**Movement direction.** MOVEMENT DIRECTION is HEAD by default (BODY ignores
physical turns; HEAD TURN is the experimental variant that turns the camera
when the head is turned while moving). In first person the stick is rotated by
the head's yaw against the anchor, and the engine walks the stick's direction
as asked: `CPed::CalculateNewVelocity` measures the walk angle against the
gameplay camera and applies it unconditionally, and `PlayerControlZelda` and
`PlayerControl1stPersonRunAround` aim the body at it at once. The same engine
code serves third person, where the stick is relative to the stock camera, as
in the Vice City build.

**Head aim (D3).** First person on foot, HEAD or HEAD TURN movement,
`HeadAim` on (the default):

- While the fire button is down the ped turns to the headset's yaw, so shots
  along the ped's heading go where you look.
- Under the default control method (Standard, the mouse-camera scheme) shots
  follow the camera instead, so `CCamera::Find3rdPersonCamTargetVector`
  returns the headset's direction, from the weapon's position.
- BODY movement welds the view to the body, so turning the ped would turn the
  world. Head aim stays off there and the body aims.

**Sniper, M16 and rocket cameras (D4).** `MODE_SNIPER`, `MODE_M16_1STPERSON`,
`MODE_ROCKETLAUNCHER` and the `*_RUNABOUT` modes (plus the PC first person
camera they share a processor with) take Beta and Alpha from the headset in
`Process_Sniper`, `Process_Rocket`, `Process_M16_1stPerson` and
`Process_1rstPersonPedOnPC`, with no zoom (the eye FOV is the headset's). The
catch: the headset's world yaw is the anchor's heading plus its own turn, so an
anchor that followed this camera would spin the view up on its own. While the
camera runs the anchor therefore holds the heading the view had when it began
(`gVrScopeLatched`), and relatches on the stock camera when it ends. With
`HeadAim=0`, in BODY movement, in third person and on the 3D screen these
cameras are the stock ones (stick driven, with the game's zoom on the screen).

**Vehicles.** First person in a vehicle anchors on the driver's head bone. The
occupant's head is dropped for the draw. `VehicleFollowRotation` 0 (default)
keeps the horizon level; 1 follows the car's pitch and roll. The setting is
row CAR VIEW on the LOCOMOTION page.

## 7. Settings

All are in the `[VR]` section of `gamedata/vr_settings.ini`. `deploy.sh
--setting KEY=VAL` writes them. The menu writes the same file.

| Key | Default | Values |
| --- | --- | --- |
| `ViewMode` | 1 | 0 first person, 1 3D screen, 2 over shoulder |
| `ThirdPersonStyle` | 1 | The style View's short press returns to (1 or 2) |
| `CutsceneMode` | 3 | 0 flat cinema, 1 stereo, 2 3D subtle, 3 3D, 4 3D deep |
| `VehicleFollowRotation` | 0 | 0 level horizon, 1 follow the car's tilt |
| `HeadAim` | 1 | 1 head aim and head-driven weapon cameras, 0 off. No menu row |
| `MovementOrientation` | 1 | 0 body, 1 head, 2 head turn (experimental) |
| `TurnMode` | 0 | 0 smooth, 1 snap |
| `TurnSensitivityPercent` | 100 | 25 to 300 |
| `HeadSteeringSensitivityPercent` | 50 | 25 to 300, for head turn |
| `SnapTurnAngleDegrees` | 30 | 15 to 90 |
| `HeadBobbing` | 0 | Walking head bob in first person |
| `RefreshRate` | 72 | 72, 80, 90, 120 |
| `StickLookBehind` | 0 | R3 looks behind on foot in first person |
| `Bind*` | | Pad bindings from the CONTROLS page (`BindA`, `BindB`, ..., `BindRightStickClick`) |
| `HudWidthPercent`, `HudScalePercent`, `HudOffsetXCm`, `HudOffsetYCm` | 100, 130, 0, 0 | HUD plane |
| `GameplayHud`, `HudWeaponPanel`, `HudClock` | 1 | Read, but nothing in the engine acts on them yet; the HUD is always drawn |
| `AntiAliasing`, `GenerateMipmaps`, `Ps2AlphaTest`, `FoliageSoftness`, `DistanceFog`, `RenderScalePercent`, `MsaaSamples` | | Graphics page |

`StickCrouch` is still read but has no use: GTA III has no crouch.

## 8. Diagnostics

`vr-diag.ini` in `~/Documents/GTA3VR` (set with `deploy.sh --diag KEY=VAL`,
removed with `--diag-reset`):

| Key | Default | Effect |
| --- | --- | --- |
| `force_render` | 0 | Render and step the game with `shouldRender=false` or before focus. A synthetic head stands at 1.6 m, eyes at plus or minus 32 mm |
| `diag_yaw` | 0 | Yaw of the synthetic head in degrees (positive turns left) |
| `dump_frame` | 0 | On the Nth rendered frame write `vr-dumps/vr-eye0.bmp`, `vr-eye1.bmp`, `vr-hud.bmp` (when the menu quad is up) and `vr-state.txt` (which says whether the frame was a theater frame). Once |
| `timing` | 0 | Recognised; `[vr-perf]` is logged every 10 s regardless |
| `diag_new_game` | 0 | Start a new game from the title menu after about 5 s |

Log tags are `GTA3VR`, `GTA3VR-game` (the game's stdout) and `librw-vk`. Lines
this port adds: `[vr]` (recentre, view mode, cinema depth and why it stayed
flat, head modes, weapon cameras), `[vr-perf]` and `[vr-dump]`.

## 9. Status

Levels: **host build** (it compiles and links, or a host test passes),
**device, unattended** (it ran on the Frame with nobody wearing it, 2026-10-03,
driven by `vr-diag.ini`), **not worn** (nobody has played it).

| Item | Level |
| --- | --- |
| `assembleFrame` builds; `com.gta3vr.frame`, minSdk 30, headtracking not required | host build |
| Frame, Index and Touch bindings: component paths, hands, unique names, bumper bound | host test |
| Boots to the frontend at 72 fps, no crash | device, unattended |
| `diag_new_game` starts a game | device, unattended |
| Opening cutscene renders in 3D cinema, the eyes differ | device, unattended |
| First-person stereo gameplay: world, HUD plane, round radar, compact coronas, body hidden, 72 fps | device, unattended |
| Fixes found on the way: no TXD.IMG cache on Vulkan; 3D cinema through GTA III's `playingIntro` | device, unattended |
| Pad mapping (on foot and in vehicles), Menu as Start, View short and hold, system recentre, sfvr timing | host build (read against the Vice City build, which is worn) |
| Third person: 3D SCREEN and OVER SHOULDER, the View toggle, the held chase view, the body shown | host build, not worn |
| Head-relative movement, head aim, head-driven weapon cameras | host build, not worn |
| `VehicleFollowRotation` and its CAR VIEW row | host build, not worn |
| Menus, loading screens, driving, boats, RC vehicles, pause, audio | not worn; audio is T8 and unchecked |
| Comfort, performance in gameplay with traffic, colours | not worn |

## 10. Open items

See [STUBS.md](STUBS.md) for the full list. The ones that matter most:

- Audio on the device (T8). `sampman_android.cpp` compiles and nothing about it
  is device-checked.
- The sky is the clear colour. A world-space sky is a later job.
- `VrCutsceneCameraActor` is not read by `CCutsceneObject`, so a stereo
  cutscene filmed from an actor's eyes shows that actor's head.
- Windscreens and dynamic lights, as in section 4.
- The frontend's Island Loading row still sets LOW; picking it brings back the
  blocking island change.
- The head modes, scope cameras and third-person views assume the Standard
  control method (the default). The Classic method has not been looked at.
- Vice City raised the LOD and traffic generation distances for the headset
  (`LODDistMultiplier`, `GenerationDistMultiplier` in `CCamera`). This port has
  not; look at pop-in once it is worn.
