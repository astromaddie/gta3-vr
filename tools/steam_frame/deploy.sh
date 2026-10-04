#!/usr/bin/env bash
# Install, launch and feed GTA III VR on a Valve Steam Frame (Android in the
# Lepton container) from a Mac or Linux host.
#
#   tools/steam_frame/deploy.sh [options]
#
#   --apk FILE         the Frame APK (default: this checkout's assembleFrame
#                      output, android/app/build/outputs/apk/frame/app-frame.apk)
#   Game data. The easy way: install GTA III (app 12100) from Steam on the
#   Frame. The game reads it in place from the Steam library, so nothing has
#   to be copied. Failing that it uses a copy in ~/Documents/GTA3VR/gamedata,
#   which is what these two options make:
#   --game-dir DIR     your GTA III PC installation: copies data TEXT anim txd
#                      models audio mp3 skins movies (and gta3.ini), then this
#                      repo's gamefiles/, to the headset's
#                      ~/Documents/GTA3VR/gamedata (rsync over SSH; about
#                      1.3 GB the first time)
#   --from-steam       install the game from Steam on the Frame; no copy needed
#                      (the game reads it in place). --from-steam still copies
#                      it into Documents as a fallback (app 12100; the folder
#                      comes from the appmanifest's installdir, "Grand Theft
#                      Auto 3"; the copy runs on the headset)
#
#   Port files. The game always reads re3's own files (text, frontend art,
#   scripts and neo/ files, this repo's gamefiles/) from Documents, never
#   from Steam, so they are installed with every deploy that installs an APK
#   (the APK is built against the matching list):
#   --port-assets      install them without anything else
#   --no-port-assets   skip them on an install
#   --register-title   register ~/devkit-game/GTA3VR as a persistent Steam
#                      library title (once). When the title exists, every
#                      install also refreshes its copy of the APK.
#   --no-install       skip adb install
#   --no-launch        skip starting the game over adb
#   --logcat SECONDS   after launching, capture SECONDS of log into
#                      android/app/build/frame-logcat.txt and print the key lines
#   --diag KEY=VAL     set a key in ~/Documents/GTA3VR/vr-diag.ini
#                      (repeatable): force_render, diag_yaw, dump_frame, timing,
#                      diag_new_game. --diag-reset deletes the file.
#   --setting KEY=VAL  set a key in the [VR] section of
#                      ~/Documents/GTA3VR/gamedata/vr_settings.ini (repeatable),
#                      e.g. --setting ViewMode=1
#   --clear-dumps      remove ~/Documents/GTA3VR/vr-dumps before the run
#   --pull-dumps DIR   after the run, copy vr-dumps into DIR (PNG copies with
#                      sips on a Mac)
#   --no-adb           only SSH steps (data, title, settings); no Lepton
#                      Development needed
#
# Environment:
#   FRAME_HOST       ssh target (default steamos@192.168.1.31, the Frame on
#                    Wi-Fi; over USB networking use steamos@10.86.200.233)
#   FRAME_ADB_HOST   adb host (default: the host part of FRAME_HOST). Lepton
#                    Development publishes its adbd on <host>:5555
#   ADB              adb binary
#
# adb needs "Lepton Development" running (start it from the Frame's Steam
# library). It deletes its apps and their data when it exits; the persistent
# Steam title (--register-title) and ~/Documents/GTA3VR survive. A game
# started over adb gets no controllers and no focus from SteamVR: use it for
# logs, and launch the GTA3VR title from the Steam library to play.
#
# Saves go to the storage root's userfiles/ (~/Documents/GTA3VR/userfiles).
#
# Before an unattended run (--diag, --logcat), make sure nobody is wearing the
# headset: a launch replaces whatever they are playing.
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
package="com.gta3vr.frame"
activity="android.app.NativeActivity"
title="GTA3VR"                   # Steam devkit titles must be alphanumeric
title_apk="GTA3-VR.apk"          # fixed name: the title's argv stays valid across builds
data="Documents/GTA3VR"          # relative to the Frame user's home; /sdcard/Documents/GTA3VR in Lepton
steam_appid="12100"              # Grand Theft Auto III on Steam
# Retail folders the game needs. GTA III has audio/ (lower case), not Audio/;
# matching is case-insensitive either way.
required="data TEXT anim txd models audio mp3 skins movies"

frame_host="${FRAME_HOST:-steamos@192.168.1.31}"
adb_host="${FRAME_ADB_HOST:-${frame_host#*@}}"
adb_bin="${ADB:-${ANDROID_HOME:-$HOME/Library/Android/sdk}/platform-tools/adb}"
[[ -x "$adb_bin" ]] || adb_bin="$(command -v adb || true)"
apk=""
game_dir=""
port_assets=0
no_port_assets=0
from_steam=0
register=0
install=1
launch=1
use_adb=1
logcat_seconds=0
diag=()
diag_reset=0
settings=()
clear_dumps=0
pull_dumps=""

die() { echo "deploy.sh: $*" >&2; exit 1; }

while (($#)); do
	case "$1" in
	--apk) apk="$2"; shift 2 ;;
	--game-dir) game_dir="$2"; shift 2 ;;
	--from-steam) from_steam=1; shift ;;
	--port-assets) port_assets=1; shift ;;
	--no-port-assets) no_port_assets=1; shift ;;
	--register-title) register=1; shift ;;
	--no-install) install=0; shift ;;
	--no-launch) launch=0; shift ;;
	--logcat) logcat_seconds="$2"; shift 2 ;;
	--diag) diag+=("$2"); shift 2 ;;
	--diag-reset) diag_reset=1; shift ;;
	--setting) settings+=("$2"); shift 2 ;;
	--clear-dumps) clear_dumps=1; shift ;;
	--pull-dumps) pull_dumps="$2"; shift 2 ;;
	--no-adb) use_adb=0; shift ;;
	-h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d'; exit 0 ;;
	*) die "unknown option $1 (see --help)" ;;
	esac
done
[[ -n "$apk" ]] || apk="$repo/android/app/build/outputs/apk/frame/app-frame.apk"

ssh_frame() { ssh -o ConnectTimeout=10 -o BatchMode=yes "$frame_host" "$@"; }

# Shell-quote each argument, for passing KEY=VAL pairs through ssh.
quote_args() { local a out=""; for a in "$@"; do out="$out $(printf '%q' "$a")"; done; printf '%s' "$out"; }

# ---- Game data: persistent storage on the Frame host ------------------------
copy_port_assets() {
	local files="$repo/gamefiles"
	[[ -f "$files/TEXT/american.gxt" && -f "$files/models/x360btns.txd" ]] \
		|| die "$files lacks re3's assets (TEXT/american.gxt, models/x360btns.txd)"
	# re3's text, frontend art, scripts and neo/ files. The game reads these
	# from here even when the retail data comes from Steam. Without them the
	# custom option labels and button art are missing.
	ssh_frame "mkdir -p ~/$data/gamedata && chmod 777 ~/$data ~/$data/gamedata"
	rsync -rt --chmod=Du=rwx,Dgo=rwx,Fu=rw,Fgo=rw "$files/" "$frame_host:$data/gamedata/"
	echo "re3 gamefiles in ~/$data/gamedata ($(cd "$files" && ls | tr '\n' ' '))"
}

if [[ -n "$game_dir" ]]; then
	[[ -d "$game_dir" ]] || die "no such folder: $game_dir"
	src=()
	for name in $required; do
		# retail installs differ in case (TEXT/Text, audio/Audio)
		found="$(find "$game_dir" -maxdepth 1 -type d -iname "$name" | head -1)"
		[[ -n "$found" ]] || die "$game_dir has no $name folder: is it a GTA III PC installation?"
		src+=("$found")
	done
	ssh_frame "mkdir -p ~/$data/gamedata && chmod 777 ~/$data ~/$data/gamedata"
	for folder in "${src[@]}"; do
		name="$(basename "$folder")"
		for want in $required; do
			[[ "$(echo "$name" | tr '[:upper:]' '[:lower:]')" == "$(echo "$want" | tr '[:upper:]' '[:lower:]')" ]] && name="$want"
		done
		echo "copying $name ..."
		rsync -rt --chmod=Du=rwx,Dgo=rwx,Fu=rw,Fgo=rw "$folder/" "$frame_host:$data/gamedata/$name/"
	done
	ini="$(find "$game_dir" -maxdepth 1 -type f -iname gta3.ini | head -1)"
	if [[ -n "$ini" ]]; then
		rsync -t --chmod=Fu=rw,Fgo=rw "$ini" "$frame_host:$data/gamedata/gta3.ini"
	else
		echo "note: $game_dir has no gta3.ini; the game will use its defaults"
	fi
	port_assets=1
elif ((from_steam)); then
	# Copy on the headset: Steam's install is outside ~/Documents, which is
	# all Lepton can see. The folder name comes from the appmanifest's
	# installdir; game folders are matched case-insensitively. Re-running is
	# an rsync, so it only copies what changed.
	ssh_frame "bash -s$(quote_args "$data" "$steam_appid" "$required")" <<'SH'
set -euo pipefail
data="$1"; appid="$2"; required="$3"
steam="$HOME/.local/share/Steam/steamapps"
manifest="$steam/appmanifest_$appid.acf"
[ -f "$manifest" ] || { echo "Grand Theft Auto III (app $appid) is not installed on the Frame" >&2; exit 1; }
if [ -d "$steam/downloading/$appid" ] || ! grep -q '"StateFlags"[[:space:]]*"4"' "$manifest"; then
	echo "Steam has not finished installing GTA III yet; wait for the download to complete" >&2
	exit 1
fi
installdir="$(sed -n 's/^[[:space:]]*"installdir"[[:space:]]*"\(.*\)".*/\1/p' "$manifest" | head -1)"
[ -n "$installdir" ] || installdir="Grand Theft Auto 3"
src="$steam/common/$installdir"
[ -d "$src" ] || { echo "$src does not exist" >&2; exit 1; }
mkdir -p "$HOME/$data/gamedata"
chmod 777 "$HOME/$data" "$HOME/$data/gamedata"
for want in $required; do
	found="$(find "$src" -maxdepth 1 -type d -iname "$want" | head -1)"
	[ -n "$found" ] || { echo "$src has no $want folder" >&2; exit 1; }
	echo "copying $want ..."
	rsync -rt --chmod=Du=rwx,Dgo=rwx,Fu=rw,Fgo=rw "$found/" "$HOME/$data/gamedata/$want/"
done
ini="$(find "$src" -maxdepth 1 -type f -iname gta3.ini | head -1)"
if [ -n "$ini" ]; then
	rsync -t --chmod=Fu=rw,Fgo=rw "$ini" "$HOME/$data/gamedata/gta3.ini"
else
	echo "note: $src has no gta3.ini; the game will use its defaults"
fi
SH
	port_assets=1
fi

# re3's own files. Installed by default with the APK (they belong together),
# or on request. When it is only the default, a headset we cannot reach over SSH
# is a warning, not a failure: the install itself goes over adb.
if ((port_assets)); then
	copy_port_assets
elif ((!no_port_assets && (install || register))); then
	if ssh_frame true 2>/dev/null; then
		copy_port_assets
	else
		echo "warning: cannot reach $frame_host over SSH; re3's gamefiles were not refreshed (use --port-assets)" >&2
	fi
fi

# ---- Unattended-test switches (vr-diag.ini in the data folder) ---------------
if ((diag_reset)); then
	ssh_frame "rm -f ~/$data/vr-diag.ini"
	echo "vr-diag.ini removed"
fi
if ((${#diag[@]})); then
	ssh_frame "mkdir -p ~/$data && python3 - ~/$data/vr-diag.ini$(quote_args "${diag[@]}")" <<'PY'
import os, sys
path, pairs = sys.argv[1], sys.argv[2:]
lines = open(path).read().splitlines() if os.path.exists(path) else []
for pair in pairs:
    key, _, val = pair.partition("=")
    for i, line in enumerate(lines):
        if line.split("=")[0].strip() == key:
            lines[i] = "%s=%s" % (key, val)
            break
    else:
        lines.append("%s=%s" % (key, val))
open(path, "w").write("\n".join(lines) + "\n")
os.chmod(path, 0o666)
print("vr-diag.ini:", " ".join(lines))
PY
fi

# ---- Game settings: [VR] section of gamedata/vr_settings.ini ------------------
if ((${#settings[@]})); then
	ssh_frame "mkdir -p ~/$data/gamedata && python3 - ~/$data/gamedata/vr_settings.ini$(quote_args "${settings[@]}")" <<'PY'
import os, sys
path, pairs = sys.argv[1], sys.argv[2:]
lines = open(path).read().splitlines() if os.path.exists(path) else []
def section_of(line):
    t = line.strip()
    return t[1:-1].strip().lower() if t.startswith("[") and t.endswith("]") else None
for pair in pairs:
    key, _, val = pair.partition("=")
    start = end = None          # [start, end) is the [VR] body
    for i, line in enumerate(lines):
        name = section_of(line)
        if name is None:
            continue
        if start is not None:
            end = i
            break
        if name == "vr":
            start = i + 1
    if start is None:
        if lines and lines[-1].strip():
            lines.append("")
        lines.append("[VR]")
        start = end = len(lines)
    elif end is None:
        end = len(lines)
    for i in range(start, end):
        if lines[i].split("=")[0].strip() == key and "=" in lines[i]:
            lines[i] = "%s=%s" % (key, val)
            break
    else:
        lines.insert(end, "%s=%s" % (key, val))
open(path, "w").write("\n".join(lines) + "\n")
os.chmod(path, 0o666)
print("vr_settings.ini:", " | ".join(l for l in lines if l.strip()))
PY
fi
if ((clear_dumps)); then
	ssh_frame "rm -rf ~/$data/vr-dumps"
fi

if ((install || register)); then
	[[ -f "$apk" ]] || die "no APK at $apk (build it with tools/steam_frame/build.sh)"
fi

# ---- Persistent Steam title --------------------------------------------------
title_exists=0
ssh_frame "test -d ~/devkit-game/$title" 2>/dev/null && title_exists=1
if ((register || (title_exists && install))); then
	ssh_frame "mkdir -p ~/devkit-game/$title"
	scp -q "$apk" "$frame_host:devkit-game/$title/$title_apk"
	echo "copied the APK into ~/devkit-game/$title/$title_apk"
fi
if ((register)); then
	parms="{\"gameid\": \"$title\", \"directory\": \"/home/steamos/devkit-game/$title\", \"force_appid\": \"\", \"argv\": [\"$title_apk\"], \"env\": {}, \"settings\": {\"steam_play\": \"1\", \"compat_tool\": \"lepton\"}, \"lepton_args\": \"\"}"
	# needs the Steam client running on the Frame; it answers with success or an error
	ssh_frame "cd ~/devkit-utils && python3 steam-client-create-shortcut --parms '$parms'"
fi

((use_adb)) || exit 0
[[ -n "$adb_bin" && -x "$adb_bin" ]] || die "adb not found; set ADB"

# ---- Lepton Development over network adb --------------------------------------
"$adb_bin" start-server >/dev/null
for attempt in 1 2 3 4 5; do
	"$adb_bin" connect "$adb_host:5555" >/dev/null 2>&1 || true
	if "$adb_bin" -s "$adb_host:5555" shell true 2>/dev/null; then break; fi
	sleep $((attempt * 2))
	if ((attempt == 5)); then
		die 'cannot reach Lepton Development at '"$adb_host"':5555. Start "Lepton Development" from the Frame'"'"'s Steam library.'
	fi
done
device="$adb_host:5555"
adb() { "$adb_bin" -s "$device" "$@"; }

if ((install)); then
	adb install -r -g "$apk" | grep -i -E 'success|fail|error' || true
	# /sdcard/Documents is a plain symlink to the host's ~/Documents in Lepton
	# 2.8.14, so this grant is not needed there; it is set in case a later
	# Lepton enforces Android's storage rules.
	adb shell appops set "$package" MANAGE_EXTERNAL_STORAGE allow >/dev/null 2>&1 || true
fi

if ((launch)); then
	adb shell am force-stop "$package"
	adb logcat -c
	adb shell am start -n "$package/$activity" >/dev/null
	echo "started $package; follow it with: $adb_bin -s $device logcat -s GTA3VR GTA3VR-game librw-vk"
fi

if ((logcat_seconds > 0)); then
	out="$repo/android/app/build/frame-logcat.txt"
	mkdir -p "$(dirname "$out")"
	("$adb_bin" -s "$device" logcat -v time >"$out" & pid=$!; sleep "$logcat_seconds"; kill "$pid") 2>/dev/null || true
	echo "log: $out"
	grep -E "Steam Frame\) starting|storage root|frame: |OpenXR|xrCreateInstance|vulkan|Vulkan|swapchain|Swapchain|session state|\[vr\]|\[vr-perf\]|\[vr-dump\]|input:|FATAL|crash|abort" "$out" | head -80 || true
fi

if [[ -n "$pull_dumps" ]]; then
	mkdir -p "$pull_dumps"
	scp -q -r "$frame_host:$data/vr-dumps/." "$pull_dumps/"
	if command -v sips >/dev/null; then
		for bmp in "$pull_dumps"/*.bmp; do
			[[ -f "$bmp" ]] && sips -s format png "$bmp" --out "${bmp%.bmp}.png" >/dev/null
		done
	fi
	echo "dumps in $pull_dumps: $(ls "$pull_dumps" | tr '\n' ' ')"
fi
