#!/bin/sh
# Run the Lua-defined M3 VM safepoint guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if grep -nE 'volatile[[:space:]]+int32_t[[:space:]]+vmstate' \
    "$ROOT/src/lj_obj.h"; then
  printf '%s\n' 'vmstate must not rely on volatile; use vmstate_* helpers' >&2
  exit 1
fi

for helper in vmstate_load_acq vmstate_store_rel lj_tg_vmstate_load_acq \
  lj_tg_vmstate_store_rel; do
  if ! grep -Rqs "static LJ_AINLINE .* ${helper}" \
      "$ROOT/src/lj_obj.h" "$ROOT/src/lj_tg.h"; then
    printf '%s\n' "${helper} helper is required for vmstate publication" >&2
    exit 1
  fi
done

if hits=$(grep -RInE -- '(^|[^[:alnum:]_])(g|tg|J2G\([^)]*\)|G\(L\))->[[:space:]]*vmstate|&[[:space:]]*(g|tg|J2G\([^)]*\)|G\(L\))->[[:space:]]*vmstate' \
    "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c" "$ROOT/src/lj_profile.c" \
    "$ROOT/src/lj_trace.c" 2>/dev/null | \
    grep -Ev 'vmstate_(load|store)_acq|vmstate_store_rel|lj_tg_vmstate_' || true); \
    [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'C-side vmstate access must use vmstate helper APIs' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m3_vm_safepoint
