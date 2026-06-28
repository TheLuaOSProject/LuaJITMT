#!/bin/sh
# Guard VM-event failure stdio as a native-state boundary.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if hits=$(awk '
  /^static uint32_t vmevent_report_failure\(lua_State \*L\)$/ { allow = 1 }
  allow && /^}/ { allow = 0; next }
  /(^|[^[:alnum:]_])(fputs|fputc|fwrite|fprintf|fflush|fgets)[[:space:]]*\(/ {
    if (!allow) print FILENAME ":" FNR ":" $0
  }
' "$ROOT/src/lj_vmevent.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'VM-event stdio must stay inside vmevent_report_failure() native boundary' >&2
  exit 1
fi

for required in \
  'static void vmevent_checkstop_fresh(lua_State *L, uint32_t actions,' \
  'vmevent_checkstop_fresh(L, actions, had_stopreq)'
do
  if ! grep -qF "$required" "$ROOT/src/lj_vmevent.c"; then
    printf '%s\n' "VM-event native fresh STOPREQ guard is missing: $required" >&2
    exit 1
  fi
done

if hits=$(awk '
  /^static void vmevent_checkstop_fresh\(lua_State \*L, uint32_t actions,/ {
    in_fresh = 1
  }
  in_fresh && /^}/ { in_fresh = 0; next }
  /lj_safepoint_checkstop\(L, actions\);/ && !in_fresh { print FNR ":" $0 }
' "$ROOT/src/lj_vmevent.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'VM-event native STOPREQ checks must use fresh semantics' >&2
  exit 1
fi

if ! awk '
  /^ptrdiff_t lj_vmevent_prepare\(lua_State \*L, VMEvent ev\)/ {
    in_fn = 1; found = 1
  }
  in_fn && /if \(tv\)/ { saw_tv_guard = 1 }
  in_fn && /lj_tv_load_acq\(&tabv, tv\)/ { saw_acq = 1 }
  in_fn && /tabV\(&tabv\)/ { saw_tab = 1 }
  in_fn && /^}/ { in_fn = 0 }
  END { exit(found && saw_tv_guard && saw_acq && saw_tab ? 0 : 1) }
' "$ROOT/src/lj_vmevent.c"; then
  printf '%s\n' 'VM-event registry table lookup must guard and snapshot the slot' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m3_vmevent_native_stdio
