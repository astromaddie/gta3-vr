#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
test_source="$repo/tools/steam_frame/tests/cutscene_skip_hold_test.cpp"
out="${TMPDIR:-/tmp}/gta3-vr-cutscene-skip-test-$$"
trap 'rm -f "$out"' EXIT HUP INT TERM

# The native readiness gate must stop offering a skip after FinishCutscene
# has already advanced the loaded camera to its spline endpoint.
grep -Fq '!HasCutsceneFinished()' "$repo/src/animation/CutsceneMgr.cpp"

${CXX:-c++} -std=c++11 -Wall -Wextra -Werror \
	-I"$repo/src/skel/android" "$test_source" -o "$out"
"$out"
