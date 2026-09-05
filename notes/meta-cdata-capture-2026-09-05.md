# Cdata first-method capture, 2026-09-05

The interpreter now captures a function-valued cdata `__index` or `__newindex`
method during the existing receiver/key admission. This removes the second
receiver admission and source-SMR interval on successful first-hop dispatch.
It does not cache a method across operations or assume an immutable metatable.

The exact receiver/key snapshot retains its original authority. Under the same
source-SMR interval, the optional path captures the current base metatable,
acquires its exact lease, performs a bounded held lookup, and admits the exact
function. All values enter existing private enumerated anchors before SMR
closes. The method root is published first; the extra method/metatable leases
close before the existing receiver and arbitrary-key publication barriers.
Refusal, absent/nonfunction methods, function environments and later chain
hops keep the existing behavior. Frame construction and callbacks occur only
after every new scope closes.

The optional path is restricted to `LJ_GC2_INTERNAL_ALLOCATOR_ONLY` builds.
The function publication route uses nonthrowing queue growth and retained
recovery identities on failure. Arbitrary allocator callbacks do not have a
new unwind proof. The override preprocess/compile control confirms refusal;
it is not a custom-allocator runtime test. The publication audit is in
[the FFI diagnosis](ffi-interpreter-lifetime-cost-2026-09-05.md).

The final source is `lj_meta.c` SHA256
`c355b30c7978b31b499b8fe41fff1c03e8ff00f4a2a8293e1e4c74ee3823161e`,
against `dd2c439179b1e12564710484d8511e4cee617f7f`. All 224 tracked source/DynASM
files match across the paired trees except that file. V1 builds are retained
as historical evidence; all accepted functional and cost results use V2's
earlier extra-lease release.

Validation includes strict rooted/scalar reader, full GC traversal/recovery,
and repaired full FNEW fixtures; stock 387 interpreter and 509 JIT tests;
FFI read/write cases in both modes; metadispatch resize; and existing canonical
rooted-chain/x64 readers. The new Lua regression covers collection, reentry,
callbacks, errors, aliases, table-valued methods, replacement, and small,
aligned and Huge cdata. It passes baseline/candidate in both modes and the
two permanent M5 registrations.

The new C protocol fixture passes all 13 independently bounded schedules with
strict assertions and ASan/LSan (`detect_leaks=1`, no suppressions). It checks
exact admission counts, source/key/output aliases, each admission retry, two
metatable replacement boundaries, actual grey-queue growth, synthetic growth
failure, and callable survival after original edges are removed and full GC
completes. The canonical `m5_meta_cdata_capture_protocol` case also passes on a
fresh `ff2a6ca0` tree with this exact source and final registrations (27.647 s
including builds). The failure hook tests the allocation-failure return path;
actual OS OOM was not induced. Queue-pressure geometry uses distinct small
arenas. Manual C helper-frame transfer is supplemented by ordinary Lua/FFI
dispatch in the script. The older negative archive is not an exact one-diff
control. Earlier leak-disabled sanitizer and rejected C99 setup logs remain
separately identified.

Seven alternating fresh-process pairs per workload use default normal static
builds, the unchanged filtered harness, JIT off, `BENCH_SCALE=.005`, GC enabled,
five internal minimum rounds, and CPU 30 on a shared host. All 70 processes
complete. FFI medians fall from 1497.51 to 1241.17 ns/iteration; the paired
geometric ratio is 0.82865 (17.1% lower). Four table controls have paired
changes between -0.6% and +0.7%. This is a local interpreter optimization,
not a new full-harness or stock-parity result.

The unmodified shared-cdata hammer passes with JIT off but still fails its
line-80 native-coverage assertion with JIT on in both the exact baseline and
candidate. The initial validation driver stopped there; its failure and the
separate successful remainder are preserved. General MT cdata recording and
the broader runtime blocking dependencies remain open.

Raw builds, source inventories, failed trials, commands, limits and independent
review are in [functional evidence](evidence/meta-cdata-capture-2026-09-05/).
All performance samples are in [benchmark evidence](../bench/meta-cdata-capture-2026-09-05/).
