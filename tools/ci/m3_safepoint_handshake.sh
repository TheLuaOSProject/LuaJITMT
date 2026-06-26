#!/bin/sh
# Run the Lua-defined M3 safepoint handshake guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- '->[[:space:]]*next_tg|&[[:alnum:]_]+->[[:space:]]*next_tg|next_tg[[:space:]]*=' \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lj_safepoint.c" \
    "$ROOT/src/lib_threading.c" \
    "$ROOT/src/lj_tg.c" \
    "$ROOT/tests/t-thr-substrate.c" \
    "$ROOT/tests/t-safepoint-handshake.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw TGState next_tg access is forbidden; use lj_tg_next_* helpers' >&2
  exit 1
fi
if hits=$(grep -RInE -- '->[[:space:]]*(poll|reqmask|hs_epoch_ack)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*(poll|reqmask|hs_epoch_ack)([^[:alnum:]_]|$)' \
    "$ROOT/src"/lj_*.c "$ROOT/src"/lib_*.c "$ROOT/src"/lj_*.h 2>/dev/null | \
    grep -vF "$ROOT/src/lj_tg.h:" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw TG safepoint mirror access is forbidden; use lj_tg_* safepoint helpers' >&2
  exit 1
fi
for helper in \
  lj_tg_mark_active_acq \
  lj_tg_mark_active_rel \
  lj_tg_alloc_black_acq \
  lj_tg_alloc_black_rel
do
  if ! grep -q "$helper" "$ROOT/src/lj_tg.h"; then
    printf '%s\n' "missing TG GC mirror helper: $helper" >&2
    exit 1
  fi
done
if hits=$(grep -RInE -- '->[[:space:]]*mark_active([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*mark_active([^[:alnum:]_]|$)|->[[:space:]]*alloc[.]alloc_black([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*alloc[.]alloc_black([^[:alnum:]_]|$)' \
    "$ROOT/src"/lj_*.c "$ROOT/src"/lib_*.c "$ROOT/src"/lj_*.h 2>/dev/null | \
    grep -vF "$ROOT/src/lj_tg.h:" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw C-side TG GC mirror access is forbidden; use lj_tg_* mirror helpers' >&2
  exit 1
fi
for helper in lj_tg_in_native_acq lj_tg_in_native_rel lj_tg_in_native_store_rlx
do
  if ! grep -q "$helper" "$ROOT/src/lj_tg.h"; then
    printf '%s\n' "missing TG native-state helper: $helper" >&2
    exit 1
  fi
done
if hits=$(grep -RInE -- '->[[:space:]]*in_native([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*in_native([^[:alnum:]_]|$)' \
    "$ROOT/src"/lj_*.c "$ROOT/src"/lib_*.c "$ROOT/src"/lj_*.h 2>/dev/null | \
    grep -vF "$ROOT/src/lj_tg.h:" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw TG native-state access is forbidden; use lj_tg_in_native_* helpers' >&2
  exit 1
fi
for helper in \
  lj_tg_flags_acq \
  lj_tg_flags_store_rlx \
  lj_tg_flags_or_rlx \
  lj_tg_flags_and_rlx \
  lj_tg_flags_test_acq \
  lj_tg_flags_all_acq
do
  if ! grep -q "$helper" "$ROOT/src/lj_tg.h"; then
    printf '%s\n' "missing TG flag helper: $helper" >&2
    exit 1
  fi
done
if hits=$(grep -RInE -- '->[[:space:]]*tg_flags([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*tg_flags([^[:alnum:]_]|$)' \
    "$ROOT/src"/lj_*.c "$ROOT/src"/lib_*.c "$ROOT/src"/lj_*.h 2>/dev/null | \
    grep -vF "$ROOT/src/lj_tg.h:" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw TG flag access is forbidden; use lj_tg_flags_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*gc2[.](tg_list|n_threads)([^[:alnum:]_]|$)|gc2[[:space:]]*->[[:space:]]*(tg_list|n_threads)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lj_safepoint.c" \
    "$ROOT/src/lib_threading.c" \
    "$ROOT/src/lj_tg.c" \
    "$ROOT/src/lj_dispatch.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 TG registry access is forbidden; use gc2_tg_* helpers' >&2
  exit 1
fi
fields='hs_epoch|hs_pending|hs_actions|hs_leader|hs_signal_ns|hs_ack_latency_samples|hs_ack_latency_sum_ns|hs_ack_latency_max_ns|hs_ack_latency_buckets'
if hits=$(grep -nE -- "->[[:space:]]*gc2[.](${fields})([^[:alnum:]_]|$)|gc2[[:space:]]*->[[:space:]]*(${fields})([^[:alnum:]_]|$)" \
    "$ROOT"/src/*.c || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 handshake state access is forbidden; use gc2_hs_* helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m3_safepoint_handshake
