#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT"

pthread_section=$(sed -n '/POSIX timer thread/,/#elif LJ_PROFILE_WTHREAD/p' \
  src/lj_profile.c)

for required in \
  'static uint32_t profile_timer_stop(ProfileState *ps, lua_State *L)' \
  'lj_native_enter(L2TG(L));' \
  'pthread_join(ps->thread, NULL);' \
  'actions = lj_native_leave(L);' \
  'return actions;'
do
  if ! printf '%s\n' "$pthread_section" | grep -qF "$required"; then
    printf '%s\n' "pthread profiler stop is missing: $required" >&2
    exit 1
  fi
done

if printf '%s\n' "$pthread_section" | grep -qF 'lj_safepoint_checkstop'; then
  printf '%s\n' 'pthread profiler timer stop must return actions, not throw before cleanup' >&2
  exit 1
fi

if ! grep -qF 'LJ_FUNC uint32_t lj_profile_stop_hs(lua_State *L);' src/lj_profile.h; then
  printf '%s\n' 'missing internal profiler stop helper declaration' >&2
  exit 1
fi

if ! awk '
  /LJLIB_CF\(jit_profile_stop\)/ { inside = 1 }
  inside && /jit_profile_registry_clear\(L\);/ { cleared = NR }
  inside && /lj_safepoint_checkstop\(L, actions\);/ { checked = NR }
  inside && /^}/ {
    if (cleared && checked && checked > cleared)
      found = 1
    inside = 0
  }
  END { exit(found ? 0 : 1) }
' src/lib_jit.c; then
  printf '%s\n' 'jit.profile.stop must check STOPREQ after clearing registry anchors' >&2
  exit 1
fi

if hits=$(awk '
  /^static void jit_profile_callback\(lua_State \*L2, lua_State \*L,/ {
    inside = 1
  }
  inside && /!lj_state_tryclaim\(L2,/ { claimfail = 1; next }
  inside && claimfail && /^[[:space:]]*$/ { next }
  inside && claimfail {
    if (/exit[[:space:]]*\(/) print FILENAME ":" FNR ":" $0
    claimfail = 0
  }
  inside && /^}/ { inside = 0; claimfail = 0 }
' src/lib_jit.c); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'jit.profile callback must drop busy hidden-coroutine samples, not exit' >&2
  exit 1
fi

if hits=$(awk '
  /^static void jit_profile_callback\(lua_State \*L2, lua_State \*L,/ {
    inside = 1
  }
  inside && /exit[[:space:]]*\(/ {
    print FILENAME ":" FNR ":" $0
  }
  inside && /^}/ {
    inside = 0
  }
' src/lib_jit.c); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'jit.profile callback errors must stop profiling, not exit the process' >&2
  exit 1
fi

if ! awk '
  /^static void jit_profile_callback\(lua_State \*L2, lua_State \*L,/ {
    inside = 1
  }
  inside && /status = lua_pcall/ {
    pcall = NR
  }
  inside && /lj_profile_stop_hs\(L\)/ {
    stopped = NR
  }
  inside && /jit_profile_registry_clear\(L\)/ {
    cleared = NR
  }
  inside && /^}/ {
    inside = 0
  }
  END {
    exit(pcall && stopped > pcall && cleared > stopped ? 0 : 1)
  }
' src/lib_jit.c; then
  printf '%s\n' 'jit.profile callback error path must stop profiling and clear registry anchors' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m5_profile_stop_native
