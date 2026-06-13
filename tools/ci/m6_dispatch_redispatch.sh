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
  'Secondary TGs interpret until RID_DISPATCH is local'
do
  if ! rg -F -q "$needle" "$ROOT/src/vm_x64.dasc"; then
    echo "guardrail: missing x64 dispatch-localization marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'DISPATCH_J\(' "$ROOT/src/vm_x64.dasc" | rg -v '#define DISPATCH_J'; then
  echo "guardrail: x64 VM must load jit_State through tg->gl->jitp" >&2
  exit 1
fi

if rg -n 'TG_DISP2[JG]' "$ROOT/src/vm_x64.dasc" |
    rg -v '#define DISPATCH_[GJ]|TGPOLL'; then
  echo "guardrail: x64 VM must not derive g/J from fixed TG dispatch offsets" >&2
  exit 1
fi

if rg -n 'DISPATCH_GL\(' "$ROOT/src/vm_x64.dasc" |
    rg -v '#define DISPATCH_GL|TGPOLL'; then
  echo "guardrail: x64 VM DISPATCH_GL use is limited to transitional TGPOLL" >&2
  exit 1
fi

echo "M6 dispatch redispatch guard passed"
