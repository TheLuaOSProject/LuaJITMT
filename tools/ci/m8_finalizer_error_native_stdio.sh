#!/bin/sh
# Guard the default finalizer-error reporter as a native-state boundary.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if hits=$(awk '
  /^static void aux_finalizer_error_report\(lua_State \*L, const char \*s\)$/ { allow = 1 }
  allow && /^}/ { allow = 0; next }
  /^static int error_finalizer\(lua_State \*L\)$/ { errfin = 1 }
  errfin && /^}/ { errfin = 0; next }
  /ERROR in finalizer/ && !allow { print FILENAME ":" FNR ":" $0 }
  errfin && /(^|[^[:alnum:]_])(fputs|fputc|fflush)[[:space:]]*\(/ {
    print FILENAME ":" FNR ":" $0
  }
' "$ROOT/src/lib_aux.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'finalizer error stdio must stay inside aux_finalizer_error_report() native boundary' >&2
  exit 1
fi

helper=$(sed -n '/^static void aux_finalizer_error_report/,/^}/p' "$ROOT/src/lib_aux.c")
case "$helper" in
  *"lj_native_enter(L2TG(L));"*"lj_native_leave(L);"*) ;;
  *)
    printf '%s\n' 'aux_finalizer_error_report() must enter and leave native state' >&2
    exit 1
    ;;
esac

exec "$ROOT/tools/ci/lua_test.sh" m8_finalizer_error_native_stdio
