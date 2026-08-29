# JIT table FLOAD forwarding and GC-defer exits (2026-07-04)

## Finding

The lockless runtime made table storage headers mutable, so the earlier JIT
change disabled CSE for `tab.array`, `tab.node`, `tab.asize`, and `tab.hmask`
loads completely. That was correct for active-MT structural races, but too
conservative for pre-MT and non-structural table-store traces. In
`tab_store_existing`, the trace kept reloading the table array/asize pair in
the hot loop, while stock hoists the pair before `LOOP`.

Fresh string/new-key insertion remains a separate GC/string-intern bottleneck.
At `BENCH_SCALE=0.1`, `tab_insert_newkey` is still about 3.71x stock and fails
the 3.0x stock gate. This patch does not mask that result.

## Fix

`lj_opt_fwd_tab_fload()` now forwards table storage/header FLOADs within the
current poll region when no same-table structural operation can invalidate the
header. It stops across XPOLL/XBAR, `NEWREF`, and table.clear boundaries through
the existing FLOAD alias limit, and it treats active/entering-MT ASTORE/HSTORE
as boundaries for same/may-alias tables because helper revalidation may
republish storage after a concurrent shape change.

GC-deferred trace exits now resume in the interpreter like atomic/finalize GC
exits. The trace exit path runs one bounded GC2 fixpoint round for deferred root
handshakes and avoids recording a hot side trace that can stitch back to the
same due GC check.

## Verification

Focused passing checks:

- `make -C src -j$(nproc)`
- `tools/ci/lua_test.sh m5_jit_table_fload_mutable m6_jit_href_nodehdr m6_jit_hrefk_nodehdr m6_jit_aref_pair_boundary`
- `tools/ci/lua_test.sh m6_jit_table_store_helper m6_jit_gcstep_pacing m9_trace_hard_assist_cadence`
- `tools/ci/lua_test.sh m9_bench_smoke m9_bench_regression`
- `BENCH_SCALE=0.01 LJ_BENCH_STOCK_FILTERS=tab_store_existing tools/ci/lua_test.sh m9_bench_stock_compare`
- `BENCH_SCALE=0.1 LJ_BENCH_STOCK_FILTERS=tab_hash_write tools/ci/lua_test.sh m9_bench_stock_compare`
- `git diff --check`

Stock comparison results:

- `tab_store_existing`: 2.020635x stock at scale 0.01, passing the 3.0x gate.
- `tab_hash_write`: 1.692308x stock at scale 0.1, passing the 3.0x gate.
- `tab_insert_newkey`: 3.707872x stock at scale 0.1, still failing the 3.0x
  gate and left as the next GC/string-intern optimization target.
