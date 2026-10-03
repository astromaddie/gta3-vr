# Stubs and open hooks after T3

T3 gets the full game target to link. These are the places where something is
stubbed, or where the Android code exists but nothing in the engine calls it
yet. Each one is picked up by the task named beside it.

## Stubbed so the target links

| Where | What it does now | Real work |
| --- | --- | --- |
| `src/skel/android/QuestProfiler.cpp` | All 19 functions of `QuestProfiler.h` are no-ops, the snapshot is zeroed | Not planned. The profiler page and its engine hooks are not carried. |
| `CRenderer::SetVrViewCameraPosition` (`renderer/Renderer.{h,cpp}`) | Stores the eye position, nothing reads it | T5: all-round culling |
| `androidgame::VrPushDynamicLights` (`vrmenu.cpp`) | Hands the backend an empty point-light list | Only if dynamic lights come back (the LIGHTING page is not carried) |
| `androidgame::VrScopeZoomFactor` (`vrmenu.cpp`) | Always 1.0 | Decision D4: no zoom in the headset, so likely stays |

## Android code that the engine does not call yet

- `VrApplyEyeFov`, `VrCaptureViewWindow`, `VrUpdateFirstPersonAnchor(true)`,
  `VrRestoreFov` and the `gVr*` globals (`android.cpp`) are not wired into
  `main.cpp`, `Renderer`, `Ped` or `Draw`. Only the `CapturePad` call of
  `VrUpdateFirstPersonAnchor(false)` and `VrRestoreFov` (from `Step`) are live.
  T5 hooks the render loop, culling, HUD, coronas and radar; T7 the hide-body
  and pad hooks.
- The HUD accessors (`VrGameplayHudEnabled`, `VrGetGameplayHudSettings`,
  `VrHudWeaponPanelEnabled`, `VrHudClockEnabled`, `VrDistanceFogEnabled`) have no
  callers yet. T5.
- `VrCutsceneCameraActor` is not read by `CCutsceneObject`. T6.
- `androidgame::VrVehicleFollowRotation` reads `[VR] VehicleFollowRotation`
  (default 0). It is used by the anchor code; there is no menu row for it.
- Wrist-panel code in `android_main.cpp` is fenced off by `GTA3VR_WRIST_PANELS`
  (0). It needs the wrist accessors, which are declared under the same flag in
  `android.h` but not defined.

## Added or changed in the engine (all `RW_VULKAN && __ANDROID__`)

`config.h`, `common.h`, `crossplatform.{h,cpp}`, `CdStream_posix.cpp`,
`fakerw/fake.cpp`, `TxdStore.cpp`, `CutsceneMgr.h` (`GetNumCutsceneObjects`),
`Renderer.{h,cpp}`, plus the compile fixes the plan did not list:
`ControllerConfig.h`, `Pad.cpp`, `Frontend.cpp`, `Game.cpp`, `re3.cpp` (the
re3 sources assume GL3 or D3D9 for pad and mouse code, and the old
`MatFX::envMap*` switches that the Vulkan librw fork replaced with
`modulateEnvMap`).

## Audio

`sampman_android.cpp` is adapted to re3's `sampman.h` and compiles, but nothing
about it is device-checked. User MP3s are not scanned. The sample table reads
`audio/sfx.SDT` and `audio/sfx.RAW`; the streams go to the Android decoder
through `platform_android`. T8.
