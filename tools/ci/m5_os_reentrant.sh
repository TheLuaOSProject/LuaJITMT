#!/bin/sh
# Guard M5 POSIX os.date/tmpname reentrant paths.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

if ! awk '
  /LJLIB_CF\(os_date\)/ { indate = 1 }
  indate && /gmtime_r/ { utc = 1 }
  indate && /localtime_r/ { local = 1 }
  /LJLIB_CF\(os_time\)/ { indate = 0 }
  END { exit(utc && local ? 0 : 1) }
' "$ROOT/src/lib_os.c"; then
  echo "guardrail: os.date must use gmtime_r/localtime_r on POSIX" >&2
  exit 1
fi

if ! awk '
  /static int os_native_mkstemp/ { infn = 1 }
  infn && /mkstemp/ { mk = 1 }
  infn && /^}/ { infn = 0 }
  END { exit(mk ? 0 : 1) }
' "$ROOT/src/lib_os.c"; then
  echo "guardrail: POSIX os.tmpname path must use mkstemp" >&2
  exit 1
fi

if ! awk '
  /LJLIB_CF\(os_setlocale\)/ { inloc = 1 }
  inloc && /la_load32_acq\(&G\(L\)->mt_active\)/ { active = 1 }
  inloc && /os\.setlocale mutation disabled after threading activation/ { err = 1 }
  inloc && /setlocale\(opt, str\)/ { call = 1 }
  /#include "lj_libdef.h"/ { inloc = 0 }
  END { exit(active && err && call ? 0 : 1) }
' "$ROOT/src/lib_os.c"; then
  echo "guardrail: os.setlocale mutation must be blocked after threading activation" >&2
  exit 1
fi

LJ_M5_OS_THREADS="${LJ_M5_OS_THREADS:-8}" \
LJ_M5_OS_ITERS="${LJ_M5_OS_ITERS:-200}" \
  "$ROOT/src/luajit" -joff "$ROOT/tests/t-os-reentrant.lua"

echo "M5 OS reentrant tests passed"
