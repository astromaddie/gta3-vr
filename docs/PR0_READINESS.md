# PR0 readiness: GTA III VR

**Disposition: HOLD · EXPERIMENTAL · PRIVATE.** Inherited engine/source rights and commercial asset provenance remain unresolved. Historical gamefiles/*.txd are present in reachable history; the audit also lists unclassified binary private-key-marker candidates. The audit does not grant permission: [2026-10-11 PR0 audit](https://github.com/astromaddie/steam-frame-vr-playbook/blob/main/reports/pr0-2026-10-11/README.md#material-findings).

## Source identity and checkout evidence

- Repository: gta3-vr; branch steam-frame; source HEAD at review: 46af61c7539f575fec8e84ab59c4fd5b59f03c34.
- Remotes: upstream is mrxenginner/re3; personal is the private target astromaddie/gta3-vr.
- The initial checkout contained untracked scratchpad material: four scratchpad/dumps/perfexp/gta3-dd150-* and gta3-dd175-* evidence folders (logs, two eye images, pointer image and state text in each), plus scratchpad/run-once.sh, run-perf-exp.sh and run-round3.sh. All were preserved. No clean isolated checkout was made.
- The previous root README is archived byte-for-byte at docs/README_PRE_PR0.md.

## Platform and build route

Target is Valve Steam Frame, Lepton Android 11/API 30, arm64-v8a, SteamVR OpenXR and Vulkan multiview; app id com.gta3vr.frame, persistent title GTA3VR. Requirements documented in docs/STEAM_FRAME.md: JDK 17 or 21, Android SDK 35, NDK 27.2.12479018 and CMake 3.22.1. From repo root, tools/steam_frame/build.sh runs :app:assembleFrame and writes android/app/build/outputs/apk/frame/app-frame.apk; tools/steam_frame/deploy.sh --register-title deploys/registers it. No build, test, package or device action was run for this documentation pass.

## Data and historical evidence

The port requires external retail game files; tracked and historical port assets still require provenance review. It reads Steam app 12100 from the Frame if installed. A fallback PC copy lives in /sdcard/Documents/GTA3VR/gamedata and needs retail folders data, TEXT, anim, txd, models, audio, mp3, skins and movies; code checks models/gta3.img, models/gta3.dir and data/gta3.dat. Steam-in-place was not device-verified in the existing status record; the fallback and port files were tested through host checks.

docs/VR_ARCHITECTURE.md records unattended boots, new-game startup, first-person stereo, view modes and 72 fps in the intro/gameplay path. Three historical wears occurred through 2026-10-05. The third found camera follow working, but also a clipped laser dot, oversized/central tooltips, HUD scale concerns and a refresh/display mismatch. Resulting fixes are documented as host-built and headset-unverified. Driving had earlier control problems; the revised layout was not re-worn. Menus, loading, boats, RC vehicles, pause, audio acceptance, comfort and gameplay performance under traffic remain open. None of that evidence certifies this HEAD.

## Rights and pending acceptance

The source root's inherited re3 notice says the authors do not feel able to grant a general code license; no blanket rights are inferred for this port, reconstructed source, historical textures or retail game files. Resolve provenance and the binary-marker inventory before any publication decision.

After a clean, identified source snapshot is built, test actual Steam Frame input on foot and in vehicles, tracking/recenter, both-eye stereo and HUD depth, laser/menu selection, cutscenes, pause, boats/RC vehicles, save/load persistence and clean exit. Re-wear the current pointer/HUD/refresh fixes. Measure real gameplay performance with traffic and report wearer comfort separately. No current-HEAD build, full-play or worn-performance claim is made.
