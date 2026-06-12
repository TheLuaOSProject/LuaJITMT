#!/bin/sh
# Run M4 VM shutdown handling for unjoined parked Lua threads.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
TMPDIR=${TMPDIR:-/tmp}
MARKER=$(mktemp "$TMPDIR/lj-m4-shutdown.XXXXXX")
rm -f "$MARKER"
trap 'rm -f "$MARKER"' EXIT HUP INT TERM

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-threading-shutdown.lua" "$MARKER"

test -f "$MARKER"
grep -qx "false" "$MARKER"
grep -q "thread interrupted: VM shutdown" "$MARKER"

echo "M4 threading shutdown tests passed"
