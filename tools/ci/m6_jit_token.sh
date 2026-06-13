#!/bin/sh
# Guard M6 JIT recorder token scaffold.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
OUT=${TMPDIR:-/tmp}/lj_t-jit-token

make -C "$ROOT/src" >/dev/null

for needle in \
  'uint32_t jit_token' \
  'lj_jit_token_try(jit_State *J)' \
  'tg != g->main_tg' \
  'Temporary until x64 RID_DISPATCH addressing is localized' \
  'la_cas32(&g->jit_token, &expect, tg->tid, LA_ACQ_REL, LA_ACQ)' \
  'void LJ_FASTCALL lj_trace_hot(jit_State *J, const BCIns *pc, lua_State *L)' \
  'lj_snap_restore_exit(jit_State *J, void *exptr, lua_State *L,' \
  'int LJ_FASTCALL lj_trace_exit(jit_State *J, void *exptr, lua_State *L,' \
  'int jit_exitcode' \
  'G2TG(g)->jit_exitcode' \
  'tg->jit_exitcode' \
  'Secondary TGs interpret until RID_DISPATCH is local' \
  'J->L = L;' \
  'lj_jit_token_held(J)' \
  'lj_jit_token_release(J)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_obj.h" "$ROOT/src/lj_trace.h" \
      "$ROOT/src/lj_trace.c" "$ROOT/src/lj_dispatch.c" \
      "$ROOT/src/lj_snap.h" "$ROOT/src/lj_snap.c" "$ROOT/src/lj_tg.h" \
      "$ROOT/src/lj_err.c" "$ROOT/src/vm_x64.dasc"; then
    echo "guardrail: missing recorder token marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'while .*jit_token|la_futex_wait\(&g->jit_token|la_futex_wait\([^)]*jit_token' \
    "$ROOT/src"; then
  echo "guardrail: recorder token must never block or spin-wait" >&2
  exit 1
fi

if rg -n '\+\+snap->count' "$ROOT/src/lj_trace.c"; then
  echo "guardrail: side-exit counters must not advance before token acquisition" >&2
  exit 1
fi

if awk '
  /->vm_hotloop:/ { hotloop = 1 }
  /->vm_callhook:/ { hotloop = 0 }
  /Stitch a new trace to the previous trace/ { stitch = 1 }
  /call extern lj_dispatch_stitch/ { stitch = 0 }
  (hotloop || stitch) && /DISPATCH_J\(L\)/ { bad = 1; print }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 hotloop/stitch must not write J->L before token acquisition" >&2
  exit 1
fi

if awk '
  /Stitch a new trace to the previous trace/ { stitch = 1 }
  /call extern lj_dispatch_stitch/ { stitch = 0 }
  stitch && /DISPATCH_J\(exitno\)/ { bad = 1; print }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 stitch must not write J->exitno before token acquisition" >&2
  exit 1
fi

if awk '
  /->vm_exit_handler:/ { exitpath = 1 }
  /->vm_exit_interp:/ { exitpath = 0 }
  exitpath && /\|\.if X64WIN/ { winonly = 1 }
  winonly && /\|\.else/ { winonly = 0 }
  winonly && /\|\.endif/ { winonly = 0 }
  exitpath && !winonly && /DISPATCH_J\((L|parent|exitno)\)/ { bad = 1; print }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64/POSIX trace exit restore state must stay call-local before side-token acquisition" >&2
  exit 1
fi

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-jit-token.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"

echo "M6 JIT recorder token guard passed"
