#!/bin/sh
# Guard M7 FFI callback runtime scratch relocation.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'typedef LJ_ALIGN(8) struct CCallbackRuntime' \
  'lua_State **owner' \
  'CCallbackRuntime cb;' \
  'lj_ccallback_enter(CTState *cts, void *cf,' \
  'lj_ccallback_leave(CTState *cts, TValue *o,' \
  'callback_conv_args(CTState *cts, lua_State *L, CCallbackRuntime *cb)' \
  'callback_conv_result(CTState *cts, lua_State *L, TValue *o,' \
  'callback_owner_claim(owner, top, L)' \
  'cb->slot = ~0u' \
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

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-callback-runtime.lua" \
  "${LJ_M7_FFI_CBACK_RT_THREADS:-6}" "${LJ_M7_FFI_CBACK_RT_ITERS:-220}"
"$ROOT/src/luajit" "$ROOT/tests/stock/test/lib/ffi/ffi_callback.lua"

echo "M7 FFI callback runtime guard passed"
