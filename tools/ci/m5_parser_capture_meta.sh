#!/bin/sh
# Build and guard parser captured-local metadata without enabling cell emission.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" XCFLAGS="-DLUA_USE_ASSERT" >/dev/null

"$ROOT/src/luajit" "$ROOT/tests/t-parser-capture-meta.lua"

for needle in \
  'VSTACK_VAR_CAPTURED' \
  'var_mark_captured(fs, reg)' \
  'unmarked captured local'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_parse.c"; then
    echo "guardrail: missing parser capture metadata marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'BC_CNEW|BC_CGET|BC_CSET' "$ROOT/src/lj_parse.c"; then
  echo "guardrail: parser capture metadata slice must not emit cell bytecode" >&2
  exit 1
fi

if rg -n '#if[[:space:]]+LJ_MT|#ifdef[[:space:]]+LJ_MT|LUAJIT_THREADSAFE' \
  "$ROOT/src/lj_parse.c"
then
  echo "guardrail: parser capture metadata must not be hidden behind LJ_MT" >&2
  exit 1
fi

echo "M5 parser captured-local metadata guard passed"
