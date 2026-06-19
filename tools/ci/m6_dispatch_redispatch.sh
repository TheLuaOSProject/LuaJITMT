#!/bin/sh
# Guard M6 dispatch-template changes are handed to attached TGs.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t_safepoint_handshake

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null
"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-safepoint-handshake.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
"$OUT"

for needle in \
  'uint32_t redispatch = 0' \
  'la_load32_acq(&g->gc2.n_threads) > 1' \
  'lj_tg_sync_dispatch(g)' \
  'lj_gc2_handshake(g, LJ_GC2_HS_REDISPATCH)' \
  'lj_tg_sync_dispatch_tg(g, tg)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_dispatch.c" \
      "$ROOT/src/lj_safepoint.c"; then
    echo "guardrail: missing dispatch redispatch marker: $needle" >&2
    exit 1
  fi
done

for needle in \
  'load_G TMPR' \
  'mov dword GL:TMPR->vmstate' \
  'load_J CARG1' \
  'load_DISPATCH RB' \
  'TG_OFS_DISPATCH' \
  'TGPOLL, dword [DISPATCH+DISPATCH_TG(poll)]' \
  'static void dispatch_setrecord' \
  'rec_owner = lj_trace_state_load(J) != LJ_TRACE_IDLE' \
  'dispatch_setrecord(tg->dispatch, mode)'
do
  if ! rg -F -q "$needle" "$ROOT/src/vm_x64.dasc" "$ROOT/src/lj_dispatch.c"; then
    echo "guardrail: missing x64 dispatch-localization marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'Secondary TGs interpret until RID_DISPATCH is local' \
    "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 secondary TGs must enter localized trace dispatch" >&2
  exit 1
fi

if rg -n 'DISPATCH_[GJ]\(' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 VM must not derive g/J from fixed DISPATCH offsets" >&2
  exit 1
fi

if rg -n 'TG_DISP2[JG]' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 VM must not derive g/J from fixed TG dispatch offsets" >&2
  exit 1
fi

if rg -n 'GG_G2TGDISP|L:RB->glref.*dispatch|add DISPATCH, GG_G2TGDISP' \
    "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 VM entry must use the running TG dispatch table" >&2
  exit 1
fi

if rg -n 'GG_OFS_TGDISP|GG_G2TGDISP|TG_DISP2[JG]' \
    "$ROOT/src/lj_dispatch.h"; then
  echo "guardrail: transitional TG dispatch offset macros must stay removed" >&2
  exit 1
fi

if rg -n 'DISPMODE_REC' "$ROOT/src/lj_dispatch.c"; then
  echo "guardrail: recording dispatch must stay TG-local, not global mode" >&2
  exit 1
fi

if ! rg -F -q 'm6_dispatch_redispatch.sh' "$ROOT/tools/ci/m6_jit.sh"; then
  echo "guardrail: m6_dispatch_redispatch.sh is not wired into the M6 aggregate" >&2
  exit 1
fi

echo "M6 dispatch redispatch guard passed"
