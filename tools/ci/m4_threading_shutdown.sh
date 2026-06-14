#!/bin/sh
# Run M4 VM shutdown handling for unjoined parked Lua threads.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
TMPDIR=${TMPDIR:-/tmp}
MARKER=$(mktemp "$TMPDIR/lj-m4-shutdown.XXXXXX")
SPIN_MARKER=$(mktemp "$TMPDIR/lj-m4-shutdown-spin.XXXXXX")
rm -f "$MARKER"
rm -f "$SPIN_MARKER"
trap 'rm -f "$MARKER" "$SPIN_MARKER"' EXIT HUP INT TERM

for needle in \
  'lj_safepoint_ack_check' \
  'call extern lj_safepoint_ack_check' \
  'TGPOLL, dword [DISPATCH+DISPATCH_TG(poll)]' \
  'cmp TGPOLL, 0'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_safepoint.c" "$ROOT/src/lj_safepoint.h" "$ROOT/src/vm_x64.dasc"; then
    echo "guardrail: VM safepoints must check STOPREQ after ack: $needle" >&2
    exit 1
  fi
done

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-threading-shutdown.lua" "$MARKER" "$SPIN_MARKER"

test -f "$MARKER"
grep -qx "false" "$MARKER"
grep -q "thread interrupted: VM shutdown" "$MARKER"
test -f "$SPIN_MARKER"
grep -qx "false" "$SPIN_MARKER"
grep -q "thread interrupted: VM shutdown" "$SPIN_MARKER"

echo "M4 threading shutdown tests passed"
