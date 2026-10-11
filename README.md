# GTA III VR on Steam Frame (experimental)

This project retargets [re3](https://github.com/mrxenginner/re3), the reverse-engineered GTA III engine, to Steam Frame. It uses the Vice City VR port's Android/OpenXR layer and a Vulkan multiview librw fork to run natively as an Android ARM64 app in Lepton. See [docs/VR_ARCHITECTURE.md](docs/VR_ARCHITECTURE.md) for the implementation map and status table.

**Status: PRIVATE · EXPERIMENTAL · HOLD.** This preview does not grant rights to distribute inherited re3 source, commercial game code or assets. Historical gamefiles/*.txd remain in reachable history and rights/provenance review is unresolved. See [docs/PR0_READINESS.md](docs/PR0_READINESS.md) and the [Steam Frame PR0 audit](https://github.com/astromaddie/steam-frame-vr-playbook/blob/main/reports/pr0-2026-10-11/README.md#material-findings). The previous upstream README is preserved at [docs/README_PRE_PR0.md](docs/README_PRE_PR0.md).

## Capabilities and evidence

| Capability | Evidence recorded in the project | Limit for this source revision |
| --- | --- | --- |
| Boot, frontend, new game, stereo and cutscenes | Earlier unattended Frame runs reached the frontend, started a game and rendered first-person stereo gameplay and the intro. | Current HEAD was not rebuilt or worn in this documentation pass. |
| Player controls and driving | Three historical wears exist; early driving and camera/control problems were found and partly fixed. | Current driving bindings and many gameplay actions are only host-built or unattended; controls, menus, boats, RC vehicles, pause and full-session play are not accepted. |
| HUD, pointer and display | Third wear found a clipped laser dot, oversized tooltips, HUD sizing and refresh/eye-size issues; documented fixes are host-built and not re-worn. | Real gameplay performance with traffic, comfort and colors remain unmeasured by a wearer at this revision. |
| Stereo display | OpenXR/Vulkan multiview runs unattended. | No complete gameplay claim; Steam-in-place data route was not device-verified in the recorded status. |

## Game data

The full retail installation must be supplied separately. Historical gamefiles/*.txd textures remain a publication blocker. Install the Steam edition of Grand Theft Auto III (Steam app 12100) on the Frame and the app reads it in place, or provide a user-owned PC install at ~/Documents/GTA3VR/gamedata (Frame path /sdcard/Documents/GTA3VR/gamedata). The fallback contains retail folders data, TEXT, anim, txd, models, audio, mp3, skins and movies; startup checks for models/gta3.img, models/gta3.dir and data/gta3.dat. The port's own gamefiles/ are deployed separately. Saves and settings stay under Documents/GTA3VR.

## Build and launch

The documented target uses JDK 17 or 21, Android SDK platform 35, NDK 27.2.12479018 and CMake 3.22.1. On Apple Silicon, Rosetta is required for the documented NDK host tools. From the repository root:

    tools/steam_frame/build.sh
    # output: android/app/build/outputs/apk/frame/app-frame.apk
    tools/steam_frame/deploy.sh --register-title

The persistent title is GTA3VR, using Lepton. The build/deploy route above is documentation, not a claim that this source commit was built or installed here. See [docs/STEAM_FRAME.md](docs/STEAM_FRAME.md) for the full setup and [docs/VR_ARCHITECTURE.md](docs/VR_ARCHITECTURE.md) for evidence by capability.
