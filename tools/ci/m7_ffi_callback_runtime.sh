#!/bin/sh
# Guard M7 FFI callback runtime scratch relocation.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
TMP=${TMPDIR:-/tmp}

for needle in \
  'typedef LJ_ALIGN(8) struct CCallbackRuntime' \
  'lua_State **owner' \
  'CCallbackRuntime cb;' \
  'typedef struct CCallbackFrame' \
  'CCALLBACK_MAX_NEST' \
  'TValue *cont' \
  'CCallbackFrame frame[CCALLBACK_MAX_NEST]' \
  'lj_ccallback_enter(CTState *cts, void *cf,' \
  'lj_ccallback_leave(CTState *cts, TValue *o,' \
  'lj_ccallback_unwind(lua_State *L, TValue *cont)' \
  'was_native = (uint8_t)(tg != NULL && tg->in_native != 0)' \
  'actions = lj_native_leave(L)' \
  'callback_frame_push(L, cb,' \
  'frame->cont == cont' \
  'callback_frame_top(cb)->was_native = 0' \
  'callback_frame_pop(cb)' \
  'if (errcode)' \
  'lj_ccallback_unwind(L, frame)' \
  'callback_conv_args(CTState *cts, lua_State *L, CCallbackRuntime *cb)' \
  'callback_conv_result(CTState *cts, lua_State *L, TValue *o,' \
  'callback_owner_claim(owner, top, L)' \
  'cb->slot = ~0u' \
  'uint64_t *cbblack' \
  'ctype_cbblack_init_l(lua_State *L, CTState *cts)' \
  'lj_ctype_cb_blacklist(CTState *cts, void *func)' \
  'lj_ctype_cb_isblacklisted(CTState *cts, void *func)' \
  'la_cas64(&tab[slot], &expect, key, LA_ACQ_REL, LA_ACQ)' \
  'la_store32_rel(&cts->cbblack_all, 1)' \
  'lj_ctype_cb_blacklist(cts, (void *)cc.func)' \
  'lj_ctype_cb_isblacklisted(cts,' \
  'lj_gc_arena_markmem(g, cts->cbblack)' \
  'lj_gc2_markmem(g, cts->cbblack)' \
  'mov TG:KBASE, L:ITYPE->tg_hint' \
  'mov CBACK:KBASE->L, ITYPE' \
  'mov CBACK:KBASE->gpr[0], CARG1' \
  'mov CBACK:KBASE->stack, rax' \
  'mov rax, CBACK:KBASE->gpr[0]' \
  'movsd xmm0, qword CBACK:KBASE->fpr[0]'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing FFI callback runtime marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'CTSTATE->cb\.(gpr|fpr|stack|slot)|mov aword CTSTATE->L' \
    "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 callback trampoline must not use shared CTState scratch" >&2
  exit 1
fi

if rg -n 'cts->cb\.(gpr|fpr|stack|slot|was_native)' \
    "$ROOT/src/lj_ccallback.c" "$ROOT/src/lj_ccall.c"; then
  echo "guardrail: C callback runtime must use per-TG callback scratch" >&2
  exit 1
fi

if rg -n 'cb->was_native' "$ROOT/src/lj_ccallback.c" "$ROOT/src/lj_ctype.h"; then
  echo "guardrail: callback native state must be per callback frame" >&2
  exit 1
fi

if rg -n 'lj_tab_storebool\(L, lj_tab_set\(L, cts->miscmap|lj_tab_get\(J->L, cts->miscmap, &key\)' \
    "$ROOT/src/lj_ccall.c" "$ROOT/src/lj_crecord.c"; then
  echo "guardrail: callback blacklist must not mutate miscmap structurally" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

out="$TMP/lj_t-ffi-callback-nested-native"
"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-ffi-callback-nested-native.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$out"
"$out"

"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-callback-runtime.lua" \
  "${LJ_M7_FFI_CBACK_RT_THREADS:-6}" "${LJ_M7_FFI_CBACK_RT_ITERS:-220}"
"$ROOT/src/luajit" "$ROOT/tests/stock/test/lib/ffi/ffi_callback.lua"

echo "M7 FFI callback runtime guard passed"
