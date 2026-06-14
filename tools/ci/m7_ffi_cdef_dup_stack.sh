#!/bin/sh
# Guard duplicate ffi.cdef/string-ctype races that force worker stack growth.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'lj_state_rehome_stack(lua_State *L)' \
  'lj_arena_allocf(&tg->allocd, NULL, 0, sz)' \
  'lj_gc2_account_alloc(g, tg, (GCSize)sz)' \
  'lj_mem_freevec(g, oldst, stacksize, TValue)' \
  'LJ_FUNC int lj_state_rehome_stack(lua_State *L)' \
  'if (!lj_state_rehome_stack(L1))' \
  'L1->tg_hint = L2TG(L)'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing worker stack arena ownership marker: $needle" >&2
    exit 1
  fi
done

if ! awk '
  /lj_tg_init_thread\(G\(L\), tg, L1/ { init = NR }
  /tg->alloc.owner_tid = th->thr.tid/ { owner = NR }
  /lj_state_rehome_stack\(L1\)/ { home = NR }
  /threading_gc_enter\(L\)/ && !enter { enter = NR }
  /lj_thr_create\(&th->thr/ && !create { create = NR }
  END {
    ok = init && owner && home && enter && create &&
	 init < owner && owner < home && home < enter && home < create
    exit ok ? 0 : 1
  }
' "$ROOT/src/lib_threading.c"; then
  echo "guardrail: spawned thread stack must move to worker TG before publication/start" >&2
  exit 1
fi

for needle in \
  'lj_native_enter(L2TG(L))' \
  'lj_native_leave(L)' \
  'lj_safepoint_checkstop(L, actions)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_ctype.c"; then
    echo "guardrail: ctype parser futex waits must participate in native-state handshakes: $needle" >&2
    exit 1
  fi
done

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

timeout 30s "$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-cdef-dup-stack.lua" \
  "${LJ_M7_FFI_DUP_STACK_ROUNDS:-30}" "${LJ_M7_FFI_DUP_STACK_ITERS:-200}"

echo "M7 FFI duplicate cdef stack-growth guard passed"
