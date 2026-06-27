#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if hits=$(grep -nE 'ps->[[:space:]]*abort[[:space:]]*=|if[[:space:]]*\([[:space:]]*ps->[[:space:]]*abort[[:space:]]*\)' \
  "$ROOT/src/lj_profile.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'profile timer abort flag must use la_load32_acq/la_store32_rel' >&2
  exit 1
fi

if hits=$(grep -RInE 'pthread_mutex|profile_lock|profile_unlock|lj_profile_lock|lj_profile_unlock|CriticalSection' \
  "$ROOT/src/lj_profile.c" "$ROOT/src/lj_profile.h" "$ROOT/src/lj_dispatch.c" || true); \
  [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'profiler hook and dispatch coordination must not use mutex/critical-section locks' >&2
  exit 1
fi

for helper in \
  profile_g_load_acq profile_g_store_rel \
  profile_cb_load_acq profile_cb_store_rel \
  profile_data_load_acq profile_data_store_rel \
  profile_samples_xchg profile_samples_add \
  profile_vmstate_load_acq profile_vmstate_store_rel
do
  if ! grep -q "$helper" "$ROOT/src/lj_profile.c"; then
    printf '%s\n' "missing profiler shared-state helper: $helper" >&2
    exit 1
  fi
done

for helper in dispatchmode_load_acq dispatchmode_store_rel dispatchmode_cas
do
  if ! grep -q "$helper" "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "missing dispatchmode helper: $helper" >&2
    exit 1
  fi
done

if hits=$(grep -RInF -- '->dispatchmode' "$ROOT/src"/lj_*.c "$ROOT/src"/lib_*.c \
  "$ROOT/src"/lj_*.h 2>/dev/null | grep -vF "$ROOT/src/lj_obj.h:" || true); \
  [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'C-side dispatchmode access must use dispatchmode_* helpers' >&2
  exit 1
fi

for helper in \
  lj_state_owner_acq \
  lj_state_owner_rel \
  lj_state_owner_cas \
  lj_state_scan_epoch_acq \
  lj_state_scan_epoch_rel \
  lj_state_scan_dirty_epoch_acq \
  lj_state_scan_dirty_epoch_rel
do
  if ! grep -q "$helper" "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "missing lua_State owner/scan helper: $helper" >&2
    exit 1
  fi
done

if hits=$(grep -RInE -- '->[[:space:]]*(thr_owner|scan_epoch|scan_dirty_epoch)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*(thr_owner|scan_epoch|scan_dirty_epoch)([^[:alnum:]_]|$)' \
  "$ROOT/src"/lj_*.c "$ROOT/src"/lib_*.c \
  "$ROOT/tests/t-state-owner.c" \
  "$ROOT/tests/t-thr-substrate.c" \
  "$ROOT/tests/t-gc2-traverse.c" 2>/dev/null || true); \
  [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'C-side lua_State owner/scan access must use lj_state_* helpers' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m5_state_owner
