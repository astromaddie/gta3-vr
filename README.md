# GTA III VR on Steam Frame

This is my Steam Frame version of [re3](https://github.com/mrxenginner/re3), the reverse-engineered GTA III engine. It uses the Vice City VR port's Android/OpenXR layer and a Vulkan multiview version of librw to run as an Android ARM64 app in Lepton. See the [architecture notes](docs/VR_ARCHITECTURE.md) for the implementation map.

**EXPERIMENTAL.** I made this for personal use, and I'm keeping it private for now. Retail textures remain in the Git history, and the rights to distribute the inherited re3 source and assets still need review. The [rights and cleanup notes](docs/PR0_READINESS.md) cover that work; the [earlier upstream README](docs/README_PRE_PR0.md) is preserved.

The port has first-person, over-the-shoulder and 3D screen views. Left View switches views during play and recentres when held; both grips plus right Menu opens the VR settings panel. Stereo rendering, head tracking, the minimap, menus and 3D cutscenes are part of the Frame path. The current code still has gaps in sky and cloud rendering, some gameplay actions and vehicle types, and full-session behavior; see the [known stubs](docs/STUBS.md).

## Game data

You need a copy of the retail PC game. The app can read GTA III installed through Steam on the Frame, or a copy in `~/Documents/GTA3VR/gamedata` (`/sdcard/Documents/GTA3VR/gamedata` inside Lepton). It keeps saves and settings under `~/Documents/GTA3VR`. You need to supply a full game installation.

## Build and install

Use JDK 17 or 21, Android SDK platform 35, NDK 27.2.12479018 and CMake 3.22.1. Rosetta is needed for the documented NDK tools on Apple Silicon. From the repository root:

```sh
tools/steam_frame/build.sh
```

The APK is `android/app/build/outputs/apk/frame/app-frame.apk`. Install and register the persistent `GTA3VR` title with:

```sh
tools/steam_frame/deploy.sh --register-title
```

Launch GTA3VR from the Steam library so SteamVR provides controller input. [docs/STEAM_FRAME.md](docs/STEAM_FRAME.md) covers setup and controls, [docs/VR_ARCHITECTURE.md](docs/VR_ARCHITECTURE.md) maps the implementation and limitations, and [docs/PR0_READINESS.md](docs/PR0_READINESS.md) documents the rights hold.
