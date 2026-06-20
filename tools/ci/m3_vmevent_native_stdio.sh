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

exec "$ROOT/tools/ci/lua_test.sh" m3_vmevent_native_stdio
