# GTA III VR on the Valve Steam Frame

This is GTA III (re3) running natively on the Steam Frame as an Android app in
Lepton, the Android 11 container the Frame uses. It draws in stereo with
OpenXR and Vulkan, so there is no PC involved. You need your own copy of GTA
III. The easy way is to install it on the Frame through Steam (app 12100); the
game reads it from there and there is nothing to copy. A PC install works too.

It is early. It boots, starts a new game and renders in stereo. It has been worn
three times. The first time the minimap showed no roads and the in-car buttons
were muddled. The second time (over the shoulder) the controls felt good and the
opening and cutscenes looked good, and it turned up five things: the camera came
off Claude when he got out of a car, he always faced where the camera looked
instead of turning to walk, the draw distance was too short, the help pop-up
could not be read and the HUD sat too close, and the mission title was the wrong
shape. Those were fixed in the code. The third time (2026-10-05) the camera now
stayed locked to the player whichever way the character faces, which is good and
stays as it is. It also turned up four things: the laser dot was clipped to a half
circle with a dither where it met the menu, the tooltips were too large and too
central, the HUD in general seemed wrongly sized (the minimap was fine), and the
display wasn't at the Frame's native 120 Hz and about 2160 pixels an eye.
Everything from the third wear is host build, headset-unverified: it builds and
the bindings test passes, and none of it has been tried on the headset yet.
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
runs over it). Install GTA III from Steam on the Frame if you haven't, then:

```sh
tools/steam_frame/deploy.sh --register-title
```

That is all the game data needs. The game looks for GTA III in the Steam
library when it starts and reads it in place, so nothing is copied. (Lepton can
see Steam's folder but can't write to it, which is fine: saves and settings go
to Documents.)

If GTA III isn't installed in Steam on the Frame, the game uses a copy of the
data in `~/Documents/GTA3VR/gamedata` instead. Make one from a PC install on your
Mac or Linux box:

```sh
tools/steam_frame/deploy.sh --game-dir "/path/to/Grand Theft Auto 3" --register-title
```

`--from-steam` also still works. It copies the Steam install into Documents on
the headset, as a fallback. You don't need it any more. A copy you already made
keeps working: when both exist the game prefers Steam, and uses the copy for
anything Steam doesn't have.

What a deploy does:

- Installs this repo's `gamefiles/` into `~/Documents/GTA3VR/gamedata`. Those
  are re3's own text, frontend art and scripts, and without them the option
  labels and button art are missing. The game always reads them from Documents,
  never from Steam, so every deploy that installs an APK refreshes them (they
  are tied to the build). `--port-assets` does just that step, and
  `--no-port-assets` skips it.
- Installs the APK in Lepton Development.
- Registers a persistent **GTA3VR** title in the Steam library (that's the
  `--register-title` part, only needed the first time).
- With `--game-dir` or `--from-steam`, first copies the folders the game reads
  (`data TEXT anim txd models audio mp3 skins movies`, and `gta3.ini`) into
  Documents.

To see which one the game picked, look for `[vr] game data:` in the log. It says
either `steam <folder>` or `Documents copy <folder> (no Steam install of app
12100 found)`.

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

Always, on foot or in a car:

| Input | Action |
| --- | --- |
| R Menu | Pause (Start) |
| L View, short press | Back in menus. During play, switch between first and third person |
| L View, hold 1 s | Recentre, with a buzz on both hands |
| Both grips + R Menu | The VR settings menu |
| Left stick | Move, or steer |
| Right stick | Turn and look. On the 3D screen it drives the game's own camera |
| L D-pad | The game's D-pad |

On foot (A, B, X, Y and the grips can be changed on the CONTROLS page):

| Input | Action |
| --- | --- |
| A | Sprint |
| B | Attack |
| X | Jump |
| Y | Get into a vehicle |
| Right trigger | Fire |
| Left trigger | Target |
| Bumpers | Previous and next weapon |
| Left grip | Pick up |
| Right grip | Target |
| Stick clicks | L3 and R3 (R3 looks behind, off in first person) |

In a car, bike or boat. This layout is fixed and the CONTROLS page leaves it
alone. Every action has a button of its own, so nothing doubles up:

| Input | Action |
| --- | --- |
| Right trigger | Accelerate |
| Left trigger | Brake, and reverse |
| A | Handbrake |
| Y | Get out. It does nothing else |
| X | Next radio station |
| B | Fire the vehicle's gun (tank, fire truck), or your own gun when you lean out |
| Left stick click | Horn |
| Left bumper | Look left (and lean out to the left with an Uzi) |
| Right bumper | Look right (same on the right) |
| Both bumpers | Look behind |
| Grips, right stick click | Nothing. The grips are never the handbrake here (that is A); a test keeps it that way |

The car camera mode is not on a button here (the game's own button for it is
Select, which this build never sends).

Y only gets you out once the car is nearly stopped, as in the original game,
and, as in the original game, holding Y puts the handbrake on, so the car
skids to a halt and then Claude gets out. In OVER SHOULDER the view stays on
Claude the whole time he climbs in or out: it is placed from him rather than
from the game's camera (which slides between its car and on-foot spots), and it
eases back onto the normal camera when he is done.

The layout follows the game's own controller setup. These are the buttons for
the default one (CONFIG 1); the other three keep the same actions on the pad
buttons they use, so the table above holds in all of them.

### Pointing at menus

Checked on the Frame with nobody wearing it, not worn yet. In the game's own menus (main menu, pause, options) and
in the VR menu, the right controller's aim ray works like a mouse. You see a thin
beam from the controller and a dot where it lands (cyan, amber while the trigger
is down). If the ray misses everything the beam still shows, 1.5 m long and
without a dot, so you can find the screen. In play and in cutscenes there is
nothing to point at and no pointer.

- **Dot on the surface.** The dot lies flat on the menu or screen it hit, 12 mm in
  front of it, and the beam stops about 2 cm short along the ray. SteamVR
  depth-composites quads that cross, so a dot centred on the surface was half
  behind it and clipped to a half circle with a dither (third wear). Host build,
  not worn.
- **Always on top.** The beam and dot are two small alpha-blended quad layers
  submitted last, after the screen and the VR menu panel, so SteamVR draws them
  over everything. Layer order each frame: world or the theater screen, the VR
  menu panel when it is open, beam, dot. They are never drawn into the eye
  picture.
- **The game's menus.** The ray is tested against the theater screen quad as
  submitted, and the hit becomes a pixel of the game's screen
  (`u * RsGlobal.width`, `v * RsGlobal.height`, the same mapping the 2D plane
  uses). That pixel goes into the PC frontend's mouse position
  (`CMenuManager::m_nMouseTempPosX/Y`), and the trigger is the left button in
  `CPad`'s mouse state (`CPad::UpdateMouse`). So hovering highlights an item the
  way a mouse does, a click selects it, and sliders follow the mouse's X. The
  right stick scrolls, as a mouse wheel. The game's own mouse sprite is not drawn
  while the ray is on the screen, because the dot is the cursor and two would
  double up (the map crosshair still follows it).
- **Pad still works.** A, B, the left stick and the D-pad are untouched. While
  you use them the cursor holds still, so a shaky hand does not pull the
  highlight back; the ray has to move a good way, or you click, to take over
  again. With the ray on the screen the right trigger is only the mouse click, so
  it does not also press Cross.
- **The VR menu.** The ray is tested against the panel as submitted and the hit
  becomes a row of the page. Hovering a row selects it (only when the row
  changes, so the left stick keeps working). A trigger click in the middle of a
  row does what A does. On the outer 16% at either side it steps the value down
  (left) or up (right), and holding it repeats the way L2 and R2 do. While the ray
  is on the panel the right trigger is the click and not R2 "plus"; L2 still steps
  down. Every page registers its row layout where it draws. ABOUT closes on any
  click.
- **Nearest hit wins** between the screen and the panel. The log says
  `[ui.pointer] target=frontend|vrmenu|none` when it changes.
- **Feel.** The trigger clicks with hysteresis (down at 0.6, up at 0.45). A hover
  change buzzes the right hand 0.2 for 10 ms and a click 0.5 for 30 ms. There is
  no haptics-scale setting in this port yet, so they are unscaled.

What to check on the headset: the beam over the VR menu, over the 3D screen and
over the pause menu; the dot on the row you aim at; clicking menu items and
sliders; that the D-pad and A still move the highlight with the ray resting on
the screen; that only one cursor shows. Unattended, `diag_pointer` below parks a
synthetic ray on a menu item.

### Views

Open the VR menu and go to LOCOMOTION, then VIEW.

- **THIRD: 3D SCREEN** (the default). All gameplay, on foot, in cars, boats and
  RC vehicles, plays on the cinema screen from the game's own camera. It uses
  the same 3D depth as the cutscenes. The view never moves on its own.
- **THIRD: OVER SHOULDER**. You stand where the game's chase camera is, with
  head tracking and a level horizon. The camera turns only with your own snap
  or smooth turns. Getting into or out of a vehicle keeps the view behind
  Claude at the distance it had, and it does not turn while he does it.
- **FIRST PERSON**. You are in the player's head. Movement follows where you
  look. While you hold the fire button, shots go where you look. With a sniper
  rifle, the M16 or the rocket launcher, the scope view follows your head
  too, with no zoom. In a car, the view keeps the horizon level; the CAR VIEW
  row makes it follow the car's pitch and roll instead.

**How Claude moves.** In both third person views he turns to face the way you
push the left stick, measured against the camera: push left and he turns left
and walks left, push toward you and he turns round and comes back. (The game's
PC "standard" controls used to pin him to the camera heading, so he always faced
forward and strafed. That was what you noticed.) While you fire or aim a gun
(fire or target held) he goes back to facing the camera and the stick strafes,
so shots go where the camera looks; fists and melee weapons still turn with the
stick. LOCOMOTION then THIRD PERSON MOVE switches between FACE MOVE (this, the
default) and FACE CAMERA (the old strafing). In first person there is nothing to
choose: the body keeps facing where you look, left stick sideways sidesteps
relative to your head, and shots follow your head while the fire button is held.

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
| `ThirdPersonMovement` | 0 | 0 face move (Claude turns to walk), 1 face camera (strafe). Third person only |
| `DrawDistancePercent` | 250 | 100 to 300 in steps of 25, percent of the flat game's draw distance. GRAPHICS page |
| `HudDistanceCm` | 300 | 150 to 400, how far out the HUD sits. It keeps the same size to the eye. HUD page, with HUD SCALE (`HudScalePercent`, 100; it was 130) |
| `TurnMode` | 0 | 0 smooth, 1 snap |
| `VehicleFollowRotation` | 0 | 1 makes the first-person car view follow the car's tilt |
| `HeadAim` | 1 | 0 turns off aiming and scopes by head. No menu row |
| `RefreshRate` | 120 | 72, 90 or 120. The rate the game asks for; see "Display" below. LOCOMOTION page |
| `RenderScalePercent` | 0 | 0 is automatic (whatever makes the eye image about 2160 wide), or 75 to 150. GRAPHICS page, needs a restart |

VR_ARCHITECTURE.md lists the rest.

Head aim and the head-driven scopes only work with MOVEMENT DIRECTION set to
HEAD or HEAD TURN. With BODY the view is welded to the body, so turning the
body would turn the world. Leave the game's own control method on Standard (the
default); the Classic one hasn't been looked at.

### Display

Host build, not worn. The standard for the Frame is 120 Hz at about 2160 x 2160
an eye, the panel's own size.

- **Refresh rate.** `RefreshRate` defaults to 120. When the session is focused the
  game lists the rates `XR_FB_display_refresh_rate` offers and asks for the
  highest one that is not above the setting. The log says what happened:
  `[vr] refresh: setting 120, offered {90, 120}, requested 120`. It never holds
  the picture back waiting for the display to settle, and it never asks for a
  lower rate than the compositor already runs at (SteamVR can list less than it
  runs) unless you changed the setting yourself. While you are wearing it, two
  [vr-perf] windows in a row (10 s each) under 90% of the current rate step down
  120, then 90, then 72, with `[vr] refresh fallback: ...` in the log. The
  fallback lasts for that session; the setting is untouched, and picking a rate
  again clears it. The LOCOMOTION row shows what SteamVR is really running at.
- **Your one-time step.** SteamVR only offers the rate set for this title in its
  own per-app settings, so the game can't make the Frame go to 120 on its own. In
  the headset, open SteamVR's settings for GTA III and set 120 Hz (and the
  resolution to 2160). This is part of the wear checklist: set it once, and check
  the log says `compositor 120` after `[vr] refresh:`.
- **Resolution.** The default is automatic: 2160 divided by the width the runtime
  recommends. With the runtime's own 1728 that is 125%; with SteamVR's per-app
  resolution already at 2160 it is 100%, so nothing renders at ~2700. Picking
  AUTO, 75, 100, 125 or 150 on the GRAPHICS page overrides it (it needs a
  restart). The log says `[vr] render scale: recommended 1728x1728, automatic ...
  125% ..., eye image 2160x2160`. The eye images are created at that size and the
  game renders straight into them, so there is no render-big-then-shrink step.

### HUD sizes

The HUD plane is sized from the eye's view window, so moving it from 2 m to 3 m
kept its angular size. What it kept was the 130% scale, which spread the
interface across about 63 degrees and put the clock, money and health 30 degrees
up and out. HUD SCALE now defaults to 100%, about 50 degrees across, and the help
box sits upper left at 0.65 times its old angular size. VR_ARCHITECTURE.md has the
numbers per element.

## 4. Testing with nobody wearing it

A headset sitting on a desk reports that it shouldn't render, so by default
nothing draws and the game never steps. The Frame-only switches below live in
`~/Documents/GTA3VR/vr-diag.ini`. Set them with `deploy.sh --diag KEY=VAL` and
remove them with `--diag-reset` before you play for real:

| Key | Default | Effect |
| --- | --- | --- |
| `force_render` | 0 | Render and step the game anyway. A synthetic head stands at 1.6 m, eyes 64 mm apart |
| `diag_yaw` | 0 | Yaw of that head in degrees (positive turns left) |
| `dump_frame` | 0 | On the Nth rendered frame, write `vr-dumps/vr-eye0.bmp`, `vr-eye1.bmp`, `vr-hud.bmp`, `vr-pointer.bmp` and `vr-state.txt` |
| `diag_new_game` | 0 | Start a new game from the title menu after about 5 s, with no input |
| `diag_pointer` | none | `frontend` or `vrmenu`: with no controller held, park a synthetic right-hand ray on that surface. `diag_pointer_u` and `diag_pointer_v` (0 to 1, v from the top, default 0.5) pick the point |
| `diag_vr_menu` | 0 | Open the VR menu by itself after about 3 s, on page 1 settings, 2 graphics, 3 HUD, 4 locomotion, 5 controls |
| `timing` | 0 | Nothing extra. `[vr-perf]` is logged every 10 s regardless |

To look at the pointer, dump a frame with it parked on something. With the title
menu up, aim at one of its items (the dump also shows the hover highlight, which
proves the mouse path); or open a VR menu page and aim at a row. `dump_frame`
then also writes `vr-pointer.bmp`, a flat picture of the previous frame's layers
(screen, panel, beam, dot) in submission order seen from the head, which shows
whether the beam and dot end up over the panel. It checks geometry and order,
not the runtime's compositor. Use a dump frame well after the menu has opened:

```sh
tools/steam_frame/deploy.sh --clear-dumps --diag force_render=1 \
    --diag diag_vr_menu=2 --diag diag_pointer=vrmenu --diag diag_pointer_u=0.5 \
    --diag diag_pointer_v=0.45 --diag dump_frame=600 --logcat 30 \
    --pull-dumps scratchpad/dumps/pointer
```

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
start with `[vr]`, `[vr-perf]` and `[vr-dump]`. `[vr] chase follow during
transition` appears once per car entry or exit in OVER SHOULDER, and `[vr] hud
layout` whenever the HUD's layout (theater or stereo, shape, distance) changes. Each radar tile that streams in
logs one `[radar]` line (size, depth, format, whether the TXD says it has alpha, how
many texels have alpha 0, how many distinct colours, and a few raw bytes), which says what the minimap is
being drawn from; add `radar` to the grep in a run script to see them. `vr-state.txt` says whether the
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
| Minimap shows the map after the tile blending fix; the in-car layout | builds, not run |
| The over-shoulder view staying on Claude through car entry and exit; third person turning to walk; DRAW DISTANCE; HUD DISTANCE; help box and mission title on the HUD plane | builds, not worn (second wear findings, 2026-10-04) |
| Nothing rolls with the head: HUD, crosshair, sights, coronas, flares and particles stay upright; the VR menu is world-locked and yaw-only (`[vr] roll-free billboards/HUD` in the log) | host build, not worn. Check by tilting the head |
| Reading the game from the Steam install in place, falling back to the copy in Documents | Frame, nobody wearing it, on the fallback: logs `Documents copy ... (no Steam install of app 12100 found)` and plays into the game. The Steam path has a host test only, because GTA III isn't installed in Steam on the Frame right now |
| Menu laser pointer: beam and dot as the last layers, frontend driven through the mouse path, VR menu rows hover and click | Frame, nobody wearing it (2026-10-05): with the ray parked by `diag_pointer`, the title menu row and the VR menu row under the dot highlight, and `vr-pointer.bmp` shows beam and dot over the screen and the panel. Clicking with a real controller is not worn yet |
| The camera stays locked to the player whichever way the character faces (third wear, 2026-10-05) | worn, good |
| Laser dot lies on the surface, beam stops short; tooltips 0.65x and to the left; HUD SCALE 100%; refresh request 120 with the 90/72 fallback; automatic ~2160 eye size; grips never the handbrake | host build, headset-unverified until worn (third wear findings, 2026-10-05) |
| Menus, boats, audio, comfort, performance with traffic | not tried |
