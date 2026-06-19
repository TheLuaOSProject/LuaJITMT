#!/bin/sh
# Guard M5 string.buffer rel/acq publication across shared-thread use.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}

make -C "$ROOT/src" -j"$JOBS" >/dev/null

if ! rg -n "la_loadptr_acq|la_storeptr_rel|lj_bufx_data_acq" \
  "$ROOT/src/lj_buf.h" >/dev/null; then
  echo "guardrail: lj_buf.h must expose acquire/release buffer accessors" >&2
  exit 1
fi

if rg -n "sbx->r|sbx->w|sbx->b|sbx->e" \
  "$ROOT/src/lib_buffer.c" "$ROOT/src/lib_base.c" "$ROOT/src/lj_meta.c" \
  "$ROOT/src/lj_serialize.c" "$ROOT/src/lj_cconv.c" >/dev/null; then
  echo "guardrail: shared string.buffer users must use lj_buf accessors" >&2
  exit 1
fi

if ! rg -F -q 'm5_buffer_publish.sh' "$ROOT/tools/ci/m5_concurrent_objects.sh"; then
  echo "guardrail: m5_buffer_publish.sh is not wired into the M5 aggregate" >&2
  exit 1
fi

"$ROOT/src/luajit" -joff "$ROOT/tests/t-buffer-thread-safety.lua"
"$ROOT/src/luajit" -jon "$ROOT/tests/t-buffer-thread-safety.lua"

echo "M5 string.buffer publication guard passed"
