#!/usr/bin/env bash
# Build the GTA III VR Steam Frame APK from this checkout on x86-64 Linux, macOS
# or x86-64 Ubuntu in Windows WSL2.
#
#   tools/steam_frame/build.sh [-- extra gradle args]
#
# Runs `./gradlew :app:assembleFrame` in android/ and prints the APK path.
# Build outputs stay in android/app/build (set GTA3VR_ANDROID_BUILD_DIR to
# move them).
#
# Needs: a JDK 17 or 21 (JAVA_HOME, or Homebrew openjdk@17/@21) and an Android
# SDK (ANDROID_HOME, default ~/Library/Android/sdk on a Mac) with platform 35,
# NDK 27.2.12479018 and CMake 3.22.1:
#   sdkmanager "platforms;android-35" "ndk;27.2.12479018" "cmake;3.22.1"
#
# Output: android/app/build/outputs/apk/frame/app-frame.apk
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
extra=()

while (($#)); do
	case "$1" in
	--) shift; extra=("$@"); break ;;
	-h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d'; exit 0 ;;
	*) echo "unknown option $1 (see --help)" >&2; exit 1 ;;
	esac
done

if [[ -z "${JAVA_HOME:-}" ]]; then
	for jdk in /opt/homebrew/opt/openjdk@21 /opt/homebrew/opt/openjdk@17 /usr/lib/jvm/java-21-openjdk* /usr/lib/jvm/java-17-openjdk*; do
		[[ -x "$jdk/bin/java" ]] && { export JAVA_HOME="$jdk"; break; }
	done
fi
[[ -n "${JAVA_HOME:-}" ]] || { echo "set JAVA_HOME to a JDK 17 or 21" >&2; exit 1; }
export PATH="$JAVA_HOME/bin:$PATH"
if [[ -z "${ANDROID_HOME:-}" ]]; then
	for sdk in "$HOME/Library/Android/sdk" "$HOME/Android/Sdk"; do
		[[ -d "$sdk" ]] && { export ANDROID_HOME="$sdk"; break; }
	done
fi
[[ -d "${ANDROID_HOME:-}" ]] || { echo "set ANDROID_HOME to an Android SDK" >&2; exit 1; }
[[ -d "$ANDROID_HOME/ndk/27.2.12479018" ]] \
	|| { echo "NDK 27.2.12479018 missing: sdkmanager \"ndk;27.2.12479018\"" >&2; exit 1; }

echo "sdk.dir=$ANDROID_HOME" > "$repo/android/local.properties"
(cd "$repo/android" && ./gradlew :app:assembleFrame --console=plain ${extra[@]+"${extra[@]}"})

build_dir="${GTA3VR_ANDROID_BUILD_DIR:-$repo/android/app/build}"
apk="$build_dir/outputs/apk/frame/app-frame.apk"
[[ -f "$apk" ]] || { echo "build finished but $apk is missing" >&2; exit 1; }
echo ""
echo "Steam Frame APK: $apk"
echo "Install it with: tools/steam_frame/deploy.sh --apk \"$apk\""
