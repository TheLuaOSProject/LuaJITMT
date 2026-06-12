#!/bin/sh
# Run M4 Lua-visible sequential spawn/join stress.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

LJ_M4_THREAD_STRESS_REPS=${LJ_M4_THREAD_STRESS_REPS:-1000} \
  "$ROOT/src/luajit" -joff "$ROOT/tests/t-threading-stress.lua"

echo "M4 threading stress tests passed"
