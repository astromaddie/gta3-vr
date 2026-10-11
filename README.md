# GTA III VR on Steam Frame

This is my Steam Frame version of [re3](https://github.com/mrxenginner/re3), the reverse-engineered GTA III engine. It uses the Vice City VR port's Android/OpenXR layer and a Vulkan multiview version of librw to run as an Android ARM64 app in Lepton. See the [architecture notes](docs/VR_ARCHITECTURE.md) for the implementation map.

**EXPERIMENTAL.** I made this for personal use. This is the source for my Frame port, with the original re3 notices kept. You still need your own full retail game installation. The [build and publication notes](docs/PR0_READINESS.md) record the checks and inherited resources; the [earlier upstream README](docs/README_PRE_PR0.md) is preserved.

The port has first-person, over-the-shoulder and 3D screen views. Left View switches views during play and recentres when held; both grips plus right Menu opens the VR settings panel. Stereo rendering, head tracking, the minimap, menus and 3D cutscenes are part of the Frame path. The current code still has gaps in sky and cloud rendering, some gameplay actions and vehicle types, and full-session behavior; see the [known stubs](docs/STUBS.md).

## Game data

You need a copy of the retail PC game. The app can read GTA III installed through Steam on the Frame, or a copy in `~/Documents/GTA3VR/gamedata` (`/sdcard/Documents/GTA3VR/gamedata` inside Lepton). It keeps saves and settings under `~/Documents/GTA3VR`. You need to supply a full game installation.

## Build the APK

Build on x86-64 Linux, macOS, or x86-64 Ubuntu under Windows WSL2. The Frame is the ARM64 run target; the NDK's Linux host tools are x86-64. Apple Silicon Macs need Rosetta for the pinned x86-64 tools.

On Windows, install Ubuntu with [WSL2](https://learn.microsoft.com/windows/wsl/install), then install a Linux JDK 17 or 21 and the Linux Android SDK Command-Line Tools inside Ubuntu. For example, install OpenJDK with `sudo apt update && sudo apt install -y openjdk-17-jdk`, and download/extract the Linux SDK Command-Line Tools under `$HOME/Android/Sdk`. Keep `JAVA_HOME`, `ANDROID_HOME` and the checkout in the Linux filesystem (for example, `~/src/gta3-vr`), not on a Windows-mounted path. See Android's [SDK command-line tools](https://developer.android.com/tools/sdkmanager) and [NDK downloads](https://developer.android.com/ndk/downloads).

Install JDK 17 or 21, Android SDK command-line tools (`sdkmanager`), platform 35, NDK 27.2.12479018 and CMake 3.22.1. Set `JAVA_HOME` and `ANDROID_HOME` to your own installations (`$HOME/Library/Android/sdk` on macOS or `$HOME/Android/Sdk` on Linux/WSL2). Run these commands in Bash; WSL2 uses the Linux SDK/JDK, not PowerShell or Windows installations. Accept the SDK licenses after installing packages:

```sh
mkdir -p "$HOME/src"
cd "$HOME/src"
git clone --branch steam-frame https://github.com/astromaddie/gta3-vr.git
cd gta3-vr
export JAVA_HOME="/path/to/linux-jdk-17-or-21"
export ANDROID_HOME="/path/to/linux-android-sdk"
export PATH="$ANDROID_HOME/cmdline-tools/latest/bin:$PATH"
sdkmanager --sdk_root="$ANDROID_HOME" \
  "platforms;android-35" "ndk;27.2.12479018" "cmake;3.22.1"
sdkmanager --licenses
tools/steam_frame/build.sh
```

The Frame APK is `android/app/build/outputs/apk/frame/app-frame.apk`. A full PC installation of GTA III is required to play, but it is not part of the APK build. The checked source revision and previous checks are recorded in [PR0 readiness](docs/PR0_READINESS.md).

## Supply game data, install and play

The game reads a Steam installation of GTA III on the Frame, or a copy of your own retail PC installation. To create the first **GTA3VR** library entry, follow Valve's public [SteamOS Devkit Client Title Upload](https://partner.steamgames.com/doc/steamhardware/steamframe/loadgames): pair the Frame with the client, then upload a folder containing the APK with **Name** `GTA3VR`, **Local Folder** set to that folder, **Start Command** `GTA3-VR.apk`, and **Runtime** `Android`.

```sh
mkdir -p .frame-upload/gta3
cp android/app/build/outputs/apk/frame/app-frame.apk .frame-upload/gta3/GTA3-VR.apk
```

On Windows, copy `.frame-upload/gta3` to your Windows Downloads folder (available inside WSL at `/mnt/c/Users/YOUR_WINDOWS_USERNAME/Downloads/`), then select that copied folder in the Devkit Client.

Enable SSH to the Frame and set its address; the build computer needs OpenSSH and `rsync` for transfers. If GTA III is installed through Steam on the Frame, copy the port's own text, frontend art and scripts into its persistent Documents folder:

```sh
export FRAME_HOST="steamos@FRAME_HOST_OR_IP"
tools/steam_frame/deploy.sh --port-assets --no-adb --no-install --no-launch
```

If you use a PC installation instead, copy its game data and the port files together. Under WSL2, a Windows `C:\` game folder is available below `/mnt/c/`; this example uses the common Steam install path:

```sh
tools/steam_frame/deploy.sh --game-dir "/mnt/c/Program Files (x86)/Steam/steamapps/common/Grand Theft Auto 3" \
  --no-adb --no-install --no-launch
```

The fallback copy and port files go under `~/Documents/GTA3VR/gamedata`; saves and settings are under `~/Documents/GTA3VR`. The deploy helper can refresh an existing Steam title's APK over SSH. If the Devkit Client stored it outside `~/devkit-game/GTA3VR`, upload the new APK through the client again. To play, launch **GTA3VR from the Frame's Steam library** so SteamVR supplies controller input. For a developer install via ADB, add Android SDK platform-tools (`adb`) to the build computer, start **Lepton Development** and omit `--no-adb`; an ADB launch does not provide the Steam title's controller input.

[docs/STEAM_FRAME.md](docs/STEAM_FRAME.md) covers setup and controls, [docs/VR_ARCHITECTURE.md](docs/VR_ARCHITECTURE.md) maps the implementation and limitations, and [docs/PR0_READINESS.md](docs/PR0_READINESS.md) records the source and rights audit.
