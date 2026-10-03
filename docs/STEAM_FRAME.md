# GTA III VR on the Valve Steam Frame

This is GTA III (re3) running natively on the Steam Frame as an Android app in
Lepton, the Android 11 container the Frame uses. It draws in stereo with
OpenXR and Vulkan, so there is no PC involved. You need your own copy of GTA
III, either on the Frame through Steam (app 12100) or on a PC.

It is early. It boots, starts a new game and renders in stereo, and that has
only been checked with the headset sitting on a desk. Nobody has played it yet.
[VR_ARCHITECTURE.md](VR_ARCHITECTURE.md) has the details and an honest status
table, and [PORT_PLAN.md](PORT_PLAN.md) has the reasoning.

## 1. Build

You need a JDK (17 or 21) and an Android SDK with platform 35, NDK
27.2.12479018 and CMake 3.22.1:

```sh
brew install openjdk@17
sdkmanager "platforms;android-35" "ndk;27.2.12479018" "cmake;3.22.1"
```

On a Mac the NDK's host tools are x86_64, so you need Rosetta. Then:

```sh
tools/steam_frame/build.sh
# -> android/app/build/outputs/apk/frame/app-frame.apk
```

That runs `./gradlew :app:assembleFrame` in `android/`. It finds a JDK and the
SDK by itself in the usual places, or you can set `JAVA_HOME` and
`ANDROID_HOME`. A rebuild after a small change is quick; a clean build takes
longer.

## 2. Install

Do this once. Start **Lepton Development** from the Frame's Steam library (adb
runs over it). Then, if GTA III is installed on the Frame from Steam:

```sh
tools/steam_frame/deploy.sh --from-steam --register-title
```

Or, with a PC install of the game on your Mac or Linux box:

```sh
tools/steam_frame/deploy.sh --game-dir "/path/to/Grand Theft Auto 3" --register-title
```

What that does:

- Copies the folders the game reads (`data TEXT anim txd models audio mp3 skins
  movies`, and `gta3.ini`) to `~/Documents/GTA3VR/gamedata` on the headset.
  `--from-steam` runs the copy on the Frame, because Lepton cannot see Steam's
  install folder.
- Copies this repo's `gamefiles/` over the top. Those are re3's own text,
  frontend art and scripts, and without them the option labels and button art
  are missing.
- Installs the APK in Lepton Development.
- Registers a persistent **GTA3VR** title in the Steam library (that's the
  `--register-title` part, only needed the first time).

Saves go to `~/Documents/GTA3VR/userfiles` and settings to
`gamedata/vr_settings.ini`. Both survive a Lepton reset.

After that, `tools/steam_frame/deploy.sh` on its own installs a new APK and
refreshes the title's copy of it. `FRAME_HOST` sets the SSH target (the default
is the Frame on Wi-Fi, `steamos@192.168.1.31`; over USB networking use
`steamos@10.86.200.233`). `deploy.sh --help` lists everything else.

## 3. Play

Launch **GTA3VR from the Steam library**. A game started over adb gets no
controllers and no focus from SteamVR, so use adb for logs only.

You start in the 3D screen view: the menus, and then the game itself, play on a
cinema screen in front of you. Short-press View on the left controller to step
into first person, and again to go back. The sections below say what each view
is.

### Controls

| Input | Action |
| --- | --- |
| R Menu | Pause (Start) |
| L View, short press | Back in menus. During play, switch between first and third person |
| L View, hold 1 s | Recentre, with a buzz on both hands |
| Both grips + R Menu | The VR settings menu |
| Left stick | Move |
| Right stick | Turn and look. On the 3D screen it drives the game's own camera |
| L D-pad | The game's D-pad |
| A, B, X, Y | Sprint/accelerate, fire, jump, enter or leave a vehicle. You can change these on the CONTROLS page |

On foot:

| Input | Action |
| --- | --- |
| Right trigger | Fire |
| Left trigger | Target |
| Bumpers | Previous and next weapon |
| Grips | L1 and R1 (changeable on the CONTROLS page) |

In a vehicle the right trigger is the accelerator and the left trigger is
brake and reverse, same as in the Vice City port.

### Views

Open the VR menu and go to LOCOMOTION, then VIEW.

- **THIRD: 3D SCREEN** (the default). All gameplay, on foot, in cars, boats and
  RC vehicles, plays on the cinema screen from the game's own camera. It uses
  the same 3D depth as the cutscenes. The view never moves on its own.
- **THIRD: OVER SHOULDER**. You stand where the game's chase camera is, with
  head tracking and a level horizon. The camera turns only with your own snap
  or smooth turns. Getting into or out of a vehicle holds the view still.
- **FIRST PERSON**. You are in the player's head. Movement follows where you
  look. While you hold the fire button, shots go where you look. With a sniper
  rifle, the M16 or the rocket launcher, the scope view follows your head
  too, with no zoom. In a car, the view keeps the horizon level; the CAR VIEW
  row makes it follow the car's pitch and roll instead.

Cutscenes default to CINEMA 3D, a cinema screen with depth behind it. LOCOMOTION
then CUTSCENES steps through flat, subtle, normal and deep 3D, and stereo
(inside the shot).

### Settings

They live in the VR menu and in `gamedata/vr_settings.ini`, section `[VR]`. You
can set them from the host before a launch:

```sh
tools/steam_frame/deploy.sh --no-launch --setting ViewMode=0 --setting HeadAim=1
```

The ones you are likely to want:

| Key | Default | What |
| --- | --- | --- |
| `ViewMode` | 1 | 0 first person, 1 3D screen, 2 over shoulder |
| `CutsceneMode` | 3 | 0 flat, 1 stereo, 2 subtle, 3 3D, 4 deep |
| `MovementOrientation` | 1 | 0 body, 1 head, 2 head turn (experimental) |
| `TurnMode` | 0 | 0 smooth, 1 snap |
| `VehicleFollowRotation` | 0 | 1 makes the first-person car view follow the car's tilt |
| `HeadAim` | 1 | 0 turns off aiming and scopes by head. No menu row |
| `RefreshRate` | 72 | 72, 80, 90 or 120 |

VR_ARCHITECTURE.md lists the rest.

Head aim and the head-driven scopes only work with MOVEMENT DIRECTION set to
HEAD or HEAD TURN. With BODY the view is welded to the body, so turning the
body would turn the world. Leave the game's own control method on Standard (the
default); the Classic one hasn't been looked at.

## 4. Testing with nobody wearing it

A headset sitting on a desk reports that it shouldn't render, so by default
nothing draws and the game never steps. The Frame-only switches below live in
`~/Documents/GTA3VR/vr-diag.ini`. Set them with `deploy.sh --diag KEY=VAL` and
remove them with `--diag-reset` before you play for real:

| Key | Default | Effect |
| --- | --- | --- |
| `force_render` | 0 | Render and step the game anyway. A synthetic head stands at 1.6 m, eyes 64 mm apart |
| `diag_yaw` | 0 | Yaw of that head in degrees (positive turns left) |
| `dump_frame` | 0 | On the Nth rendered frame, write `vr-dumps/vr-eye0.bmp`, `vr-eye1.bmp`, `vr-hud.bmp` and `vr-state.txt` |
| `diag_new_game` | 0 | Start a new game from the title menu after about 5 s, with no input |
| `timing` | 0 | Nothing extra. `[vr-perf]` is logged every 10 s regardless |

A typical run, first person, looking 90 degrees to the left. The new game opens
with a cutscene, so pick a dump frame well after it (the number counts rendered
frames, 72 a second):

```sh
tools/steam_frame/deploy.sh --clear-dumps --setting ViewMode=0 \
    --diag force_render=1 --diag diag_new_game=1 --diag dump_frame=6000 \
    --diag diag_yaw=90 --logcat 120 --pull-dumps scratchpad/dumps/yaw90
tools/steam_frame/tests/check_bindings.py       # host test for the controller bindings
```

Log tags are `GTA3VR`, `GTA3VR-game` and `librw-vk`. The lines this port adds
start with `[vr]`, `[vr-perf]` and `[vr-dump]`. `vr-state.txt` says whether the
dumped frame was a theater frame (menu, cutscene, 3D screen) or a stereo one.

Don't run any of this while someone is wearing the headset: a launch replaces
whatever they are playing. A quick check:
`ssh steamos@<frame> 'ps -eo etime,comm | grep -i activity'`.

## 5. What has been checked

Short version. The full table is in VR_ARCHITECTURE.md.

| Claim | Level |
| --- | --- |
| The APK builds (`com.gta3vr.frame`, minSdk 30), the bindings test passes | host |
| Boots to the frontend at 72 fps, starts a new game, the opening cutscene shows in 3D cinema, first-person stereo gameplay renders (world, HUD, radar, coronas) | Frame, nobody wearing it |
| Third person (both styles), head-relative movement, head aim, scope cameras, CAR VIEW | builds, not run |
| Menus, driving, boats, audio, comfort, performance with traffic | not tried |
