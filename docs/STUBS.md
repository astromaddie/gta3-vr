# Stubs and open hooks after T5

T3 got the full game target to link; T5 wired the stereo world. These are the places where something is
stubbed, or where the Android code exists but nothing in the engine calls it
yet. Each one is picked up by the task named beside it.

## Stubbed so the target links

| Where | What it does now | Real work |
| --- | --- | --- |
| `src/skel/android/QuestProfiler.cpp` | All 19 functions of `QuestProfiler.h` are no-ops, the snapshot is zeroed | Not planned. The profiler page and its engine hooks are not carried. |
| `CClouds::Render` (`renderer/Clouds.cpp`) | Returns at once on Vulkan, so the sky is the clear colour: no clouds, sun glow or rainbow. `RenderBackground` and `RenderHorizon` are skipped in `main.cpp` for the same reason | Port V's world-space sky later |
| `CRenderer::SetVrCullingMode` (`renderer/Renderer.h`) | Default `VR_CULLING_SAFE`; `VR_CULLING_OFF` (draw the whole 360 degree list) exists but nothing sets it | Wire to a setting or diag key if a per-view hole needs ruling out. V's AUTHORED and AGGRESSIVE occlusion modes are not carried: re3 has no occlusion system |
| `CCutsceneObject::PreRender` | The projected ped shadow is not stored on Vulkan (V: it paints the actor black) | Revisit with T6 if shadows are wanted |
| `androidgame::VrPushDynamicLights` (`vrmenu.cpp`) | Hands the backend an empty point-light list | Only if dynamic lights come back (the LIGHTING page is not carried) |
| `androidgame::VrScopeZoomFactor` (`vrmenu.cpp`) | Always 1.0 | Decision D4: no zoom in the headset, so likely stays |

## Android code that the engine does not call yet

- `VrApplyEyeFov`, `VrCaptureViewWindow`, `VrUpdateFirstPersonAnchor(true)`,
  `VrGetGameplayHudSettings`, `VrGetViewBasis` and the first-person globals
  `gVrHidePlayerBody`, `gVrFirstPersonActive`, `gVrPlayerEntity` are now live
  (`main.cpp`, `Renderer.cpp`, `WaterLevel.cpp`). Still unread: `gVrInVehicle`
  and the ped-side hide-body hooks (`CPed::PreRender`), which are T7.
- `VrGameplayHudEnabled`, `VrHudWeaponPanelEnabled`, `VrHudClockEnabled` and
  `VrDistanceFogEnabled` still have no callers (no `Hud.cpp` or fog hook was
  ported in T5).
- `VrCutsceneCameraActor` is not read by `CCutsceneObject` (head collapse for the
  actor camera). T6.
- Windscreen alpha pass: V draws vehicle windscreens without depth writes and
  wraps the alpha atomics in `rw::vulkan::setVehicleAlphaPass`
  (`RenderAlphaAtomics`). Not ported; look at it if cars read as translucent.
- `androidgame::VrVehicleFollowRotation` reads `[VR] VehicleFollowRotation`
  (default 0). It is used by the anchor code; there is no menu row for it.
- Wrist-panel code in `android_main.cpp` is fenced off by `GTA3VR_WRIST_PANELS`
  (0). It needs the wrist accessors, which are declared under the same flag in
  `android.h` but not defined.
- The frontend's "Island Loading" row and its reset-to-defaults still set
  `ISLAND_LOADING_LOW` (`MenuScreensCustom.cpp`); T5 forces HIGH at start and
  after the ini load only. If a player picks LOW the old blocking island change
  comes back.

## Added or changed in the engine (all `RW_VULKAN && __ANDROID__`)

`config.h`, `common.h`, `crossplatform.{h,cpp}`, `CdStream_posix.cpp`,
`fakerw/fake.cpp`, `TxdStore.cpp`, `CutsceneMgr.h` (`GetNumCutsceneObjects`),
`Renderer.{h,cpp}`, plus the T5 set: `main.cpp`, `Draw.cpp`
(`FindAspectRatio`; re3 has no `CalculateAspectRatio`), `Sprite2d.{h,cpp}`,
`Radar.cpp`, `Coronas.cpp`, `WaterLevel.cpp`, `Clouds.cpp`, `Collision.cpp`,
`PlayerInfo.cpp`, `Frontend.cpp` and `re3.cpp` (island loading HIGH),
`CutsceneObject.cpp`, plus the compile fixes the plan did not list:
`ControllerConfig.h`, `Pad.cpp`, `Frontend.cpp`, `Game.cpp`, `re3.cpp` (the
re3 sources assume GL3 or D3D9 for pad and mouse code, and the old
`MatFX::envMap*` switches that the Vulkan librw fork replaced with
`modulateEnvMap`).

## Audio

`sampman_android.cpp` is adapted to re3's `sampman.h` and compiles, but nothing
about it is device-checked. User MP3s are not scanned. The sample table reads
`audio/sfx.SDT` and `audio/sfx.RAW`; the streams go to the Android decoder
through `platform_android`. T8.
