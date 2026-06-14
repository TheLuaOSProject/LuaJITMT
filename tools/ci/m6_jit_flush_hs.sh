#!/bin/sh
# Guard JIT flushes use safepoint-scoped publication and retirement.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

make -C "$ROOT/src" >/dev/null

for needle in \
  'lj_trace_flushall_hs(lua_State *L)' \
  'lj_gc2_handshake(g, LJ_GC2_HS_EXIT_TRACES|LJ_GC2_HS_FLUSHJ)' \
  'lj_trace_flushall(mainthread(g));  /* 08 section 8.7 leader action. */' \
  '(void)lj_trace_flushall_hs(J->L);' \
  '(void)lj_trace_flushall_hs(L);' \
  'uint32_t lj_trace_flushscope(jit_State *J, TraceNo traceno)' \
  '(void)lj_trace_flushscope(J, lnk);  /* Flush return trace after HS. */' \
  'trace_scope_flush_dependency(jit_State *J, GCtrace *T)' \
  '(void)trace_flushscope_mark_deps(G2J(g));' \
  'trace_flushside(jit_State *J, GCtrace *T, int scoped)' \
  'return trace_flushside(J, T, 1);' \
  '(void)trace_flushside(J, T, 1);' \
  'trace_nextside_rel(root, next);' \
  'first_trace_with_root(jit_State *J, TraceNo root)' \
  'call_jit_flush_trace(L, sidetrace);'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_trace.c" \
      "$ROOT/src/lj_safepoint.c" "$ROOT/src/lj_record.c" \
      "$ROOT/src/lj_dispatch.c" "$ROOT/tests/t-vm-safepoint.c"; then
    echo "guardrail: missing JIT flush handshake marker: $needle" >&2
    exit 1
  fi
done

hits=$(rg -n -- 'lj_trace_flushall\((J->L|L)\)' \
  "$ROOT/src/lj_trace.c" "$ROOT/src/lj_record.c" \
  "$ROOT/src/lj_dispatch.c" "$ROOT/src/lj_api.c" \
  "$ROOT/src/lj_profile.c" "$ROOT/src/lib_ffi.c" || true)
if [ -n "$hits" ]; then
  echo "guardrail: full trace flush callers must route through HS_FLUSHJ" >&2
  echo "$hits" >&2
  exit 1
fi

if rg -n 'lj_trace_flush\(J, lnk\)' "$ROOT/src/lj_record.c"; then
  echo "guardrail: recorder-internal scoped return flushes must route through HS_EXIT_TRACES" >&2
  exit 1
fi

if rg -F -q 'Only root traces are considered' "$ROOT/src/lj_trace.c"; then
  echo "guardrail: numeric trace flushes must not document side traces as ignored" >&2
  exit 1
fi

if ! rg -F -q 'm6_jit_flush_hs.sh' "$ROOT/tools/ci/m6_jit.sh"; then
  echo "guardrail: m6_jit_flush_hs.sh is not wired into the M6 aggregate" >&2
  exit 1
fi

"$ROOT/tools/ci/m5_jit_trace_publish.sh"
"$ROOT/tools/ci/m3_vm_safepoint.sh"
"$ROOT/src/luajit" "$ROOT/tests/stock/test/misc/jit_flush.lua"

echo "M6 JIT flush handshake guard passed"
