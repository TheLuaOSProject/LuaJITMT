#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- 'cts[[:space:]]*->[[:space:]]*(cbblack|sizecbblack|cbblack_all)|&[[:space:]]*cts[[:space:]]*->[[:space:]]*(cbblack|sizecbblack|cbblack_all)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CTState callback blacklist access is forbidden; use ctype_cbblack_* helpers' >&2
  exit 1
fi
if hits=$(awk '
  /^static void callback_conv_args\(/ || /^static void callback_conv_result\(/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(info|size|sib)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_ccallback.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size/sib reads are forbidden in callback runtime conversion helpers; use ctype_*_acq() helpers' >&2
  exit 1
fi
if hits=$(awk '
  /^CCallbackRuntime \* LJ_FASTCALL lj_ccallback_prepare\(/ { in_fn = 1 }
  in_fn && /abort[[:space:]]*\(/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_ccallback.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'TLS-less callbacks without a legal carrier must return NULL, not abort' >&2
  exit 1
fi
if ! grep -qF 'jz ->vm_ffi_callback_dead' "$ROOT/src/vm_x64.dasc" ||
   ! grep -qF '|->vm_ffi_callback_dead:' "$ROOT/src/vm_x64.dasc"; then
  printf '%s\n' 'x64 callback trampoline must return a zero C result when prepare returns NULL' >&2
  exit 1
fi
for helper in lj_tg_ffi_call_func_acq lj_tg_ffi_call_func_rel
do
  if ! grep -q "$helper" "$ROOT/src/lj_tg.h"; then
    printf '%s\n' "missing TG FFI call-function helper: $helper" >&2
    exit 1
  fi
done
if hits=$(grep -RInE -- '->[[:space:]]*ffi_call_func([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*ffi_call_func([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_ccall.c" "$ROOT/src/lj_ccallback.c" "$ROOT/src/lj_tg.c" "$ROOT/src/lj_tg.h" 2>/dev/null | \
    grep -vF "$ROOT/src/lj_tg.h:" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw TG FFI call-function access is forbidden; use lj_tg_ffi_call_func_* helpers' >&2
  exit 1
fi
for helper in ccallback_had_stopreq ccallback_fresh_stopreq
do
  if ! grep -q "$helper" "$ROOT/src/lj_ccallback.c"; then
    printf '%s\n' "missing FFI callback native STOPREQ freshness helper: $helper" >&2
    exit 1
  fi
done
if ! grep -q 'native_had_stopreq' "$ROOT/src/lj_ctype.h" ||
   ! grep -q 'cb->native_had_stopreq = (uint8_t)had_stopreq' "$ROOT/src/lj_ccall.c" ||
   ! grep -q 'cb->native_had_stopreq' "$ROOT/src/lj_ccallback.c"; then
  printf '%s\n' 'FFI callback STOPREQ freshness must use the surrounding FFI native-entry snapshot' >&2
  exit 1
fi
if hits=$(awk '
  /^lua_State \* LJ_FASTCALL lj_ccallback_enter\(/ { in_fn = 1 }
  in_fn && /actions[[:space:]]*&[[:space:]]*LJ_GC2_HS_STOPREQ/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_ccallback.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'callback entry must use fresh STOPREQ detection, not the native-leave action mask alone' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_callback_runtime
