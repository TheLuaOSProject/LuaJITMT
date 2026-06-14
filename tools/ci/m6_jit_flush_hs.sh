#!/bin/sh
# Guard recorder-internal full flushes use the safepoint protocol.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

make -C "$ROOT/src" >/dev/null

for needle in \
  'lj_trace_flushall_hs(lua_State *L)' \
  'lj_gc2_handshake(g, LJ_GC2_HS_EXIT_TRACES|LJ_GC2_HS_FLUSHJ)' \
  'lj_trace_flushall(mainthread(g));  /* 08 section 8.7 leader action. */' \
  '(void)lj_trace_flushall_hs(J->L);' \
  '(void)lj_trace_flushall_hs(L);' \
  'trace_scope_flush_dependency(jit_State *J, GCtrace *T)' \
  '(void)trace_flushscope_mark_deps(G2J(g));'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_trace.c" \
      "$ROOT/src/lj_safepoint.c"; then
    echo "guardrail: missing recorder flush handshake marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'lj_trace_flushall\((J->L|L)\)' "$ROOT/src/lj_trace.c"; then
  echo "guardrail: recorder-internal full flushes must route through HS_FLUSHJ" >&2
  exit 1
fi

if ! rg -F -q 'm6_jit_flush_hs.sh' "$ROOT/tools/ci/m6_jit.sh"; then
  echo "guardrail: m6_jit_flush_hs.sh is not wired into the M6 aggregate" >&2
  exit 1
fi

"$ROOT/tools/ci/m5_jit_trace_publish.sh"
"$ROOT/src/luajit" "$ROOT/tests/stock/test/misc/jit_flush.lua"

echo "M6 JIT flush handshake guard passed"
