# Bounded rooted table length and active-MT JIT lowering

Date: 2026-07-25

This note records one b1.2.1 rooted-reader tranche. It does not claim that
table length, iteration, structural mutation, or the ordinary zero-wait gate is
complete.

## Problem

Stock `IR_ALEN` is appropriate while the process is single-threaded: its table
and structural vectors cannot be replaced concurrently. Once MT has become
active, however, a trace may observe a table while another actor publishes a
new array/hash generation. Following the original table or either naked vector
then lacks the exact owner, root, allocation, and paired-generation proof
required by the lockless design.

The existing `lj_tab_len_rooted()` interpreter helper retains the table and
structural generation correctly, but it is a retrying compatibility path and
can reach `lj_tab_wait_l()`. It therefore cannot be called directly from
generated code that promises bounded completion.

## Implemented transaction

`lj_tab_len_rooted_try(L, tabroot)` performs one bounded authoritative-root
attempt and returns either an exact non-negative length or
`LJ_TAB_LEN_RETRY`.

The attempt:

1. snapshots the exact TG/state/physical-actor owner;
2. enters one nonwaiting SMR read interval with `lj_gc2_smr_read_try()`;
3. loads the authoritative `TValue` root and acquires an exact table lease;
4. captures one paired array/hash generation and computes the Lua table length
   without following a subsequently published vector;
5. validates that generation at the result linearization point;
6. revalidates the exact owner and the original raw table root; and
7. releases the SMR interval and exact lease on every admitted exit.

The helper does not wait, yield, allocate, or throw. A retiring generation,
failed lease, root replacement, owner transfer, generation replacement, or
oversized result produces `LJ_TAB_LEN_RETRY` rather than consuming stale
state.

## Recorder lowering

Before MT activation, table length still emits the stock `IR_ALEN`. This keeps
the ordinary single-threaded hot trace unchanged.

After MT activation, supported table-length recording emits:

- a generated-frame `TMPREF IN1` `TValue` root for the table;
- on x64, a nonthrowing `CALLS` to `lj_tab_len_forjit_try()`; and
- a guard that side exits when the result is `LJ_TAB_LEN_RETRY`.

The generated-only helper cross-checks the dispatch `tg_hint` against the
universe-aware current TG, requires the exact `tg->tmptv` root, live JIT entry,
physical actor and state ownership, and enters an owner-written table-vector
epoch before acquiring either structural root. JLOOP keeps `jit_base`
published across ordinary generated calls, so destructive GC2 reclamation
cannot begin and `tmptv` remains an enumerated exact root. The per-TG epoch
independently prevents an old array or node generation from being reclaimed
after it is captured. Root, paired generation, carrier, JIT and owner facts are
all rechecked before returning the scalar result.

This narrower ABI removes the global SMR reader and counted table-body lease
from the generated hot path. The general `lj_tab_len_rooted_try()` remains
unchanged for adversarial C roots and non-x64 generated fallback; direct or
interpreter calls to the x64 ABI fail closed.

The common dense/no-hash shape also has an exact bounded shortcut: after the
paired snapshot it accepts `asize-1` only when the final positive array slot is
a valid non-nil Lua value, then performs the same paired-current check. Empty
tables are exact zero. A boundary hole or any hash part uses the full widening
and binary search; internal/malformed observations request retry. FINCLAIM
classification and the owner-written epoch operations are inline on this hot
path, while their existing external entry points remain available elsewhere.

The side exit replays the current bytecode in the interpreter. The bounded
helper has made no Lua-visible mutation, so replay does not duplicate an
effect. This lowering is shared by direct table `#`, Lua 5.2-compatible
`rawlen(table)`, and the default length used by the recorder paths for
`table.insert()` and `table.concat()`. Existing recorder restrictions still
apply: for example, an active-MT Lua 5.2 `#table` may fail closed earlier
during mutable metatable lookup.

## Evidence

The focused C fixture covers:

- zero and nonzero exact lengths;
- closed SMR admission and injected exact-table lease failure;
- an initially retiring array generation;
- array-generation replacement immediately before validation;
- table-root replacement;
- exact-owner loss and an invalid owner;
- bounded generated-code retry, side exit, and interpreter replay; and
- direct-call rejection by the generated-only ABI;
- exact nested table-read depth/epoch restoration; and
- balanced SMR, vector epoch, lease, root-descriptor, and dynamic-root-anchor
  accounting, with no table wait-counter movement and no helper allocation.

The JIT IR gate checks all live traces. It requires stock `IR_ALEN` and no
rooted helper before MT, then requires `TMPREF IN1` plus the rooted `CALLS` and
forbids `IR_ALEN` in the supported active-MT case. The Lua 5.2-compatible
`rawlen()` path is covered separately.

The existing resize stress also now treats only the stock-valid concurrent
`invalid key to 'next'` result from a racy `pairs()` traversal as an accepted
observer outcome. Its explicit stable invalid-cursor semantic test remains
unchanged.

Validation for this tranche includes focused and combined M5/M6 tests, strict
GCC and Clang builds, a no-JIT build, Lua 5.2 compatibility, focused ASan and
UBSan fixtures, Windows x64 cross-build plus Wine runtime/IR smoke, and macOS
x64 cross-build plus Darling runtime/IR smoke. Wider assertion/helper builds
still expose unrelated pre-existing warnings in `lj_cconv.c`, `lj_asm.c`, and
the MinGW x64 backend when globally promoted to errors; none is in a touched
file.

Those cross-platform smokes are incidental evidence for this tranche, not the
start of the release platform pass. Comprehensive macOS and Windows checking,
diagnosis, and fixes are deliberately deferred until b1.2.1 is otherwise ready
for release; current implementation work remains focused on Linux lockless
correctness and hot-path performance.

## Performance boundary

The important common-case property is exact: pre-MT traces retain stock
`IR_ALEN`, so this change adds no generated-code instruction to that path.

A nine-sample, core-pinned 20-million-iteration microbenchmark measured
2.461 ns per pre-MT `#table` and 9.332 ns for the x64 active-MT generated
path. The committed general rooted ABI measured 64.125 ns in the same run, so
the generated-only epoch path is about 6.87x faster than the first bounded
implementation and 3.79x the stock pre-MT lowering. A nearby active-MT rooted
point read remained 64.361 ns, confirming that the length improvement is the
removal of general lease/SMR machinery rather than a benchmark artifact.

This is a reproducible microbenchmark result, not yet the complete b1.2.1
three-sample suite gate. The remaining roughly 6.9 ns absolute delta is explicit
parity debt and the 3.79x ratio is still above the planned 3x gate, but the
former 25x cliff is gone and incremental optimization is now realistic.

## Linux validation

The final production-source diff fingerprint was
`849fcdffb449db02d4803478a92c386f497a443ff33b4b0c19abe291b8814df7`.
Clean focused M5 and M6 gates passed, followed by 20 repeated C-fixture runs
and five repeated JIT-IR runs. A six-writer/two-GC-worker length-and-resize
stress with 8,192 resize rounds and 4,096 observer rounds passed 30/30, and the
broader resize, traced-read, weak-finalizer, remote-stack-GC and worker-
activation matrix passed.

Both GCC and Clang built the helper/assertion profile cleanly with
`-Wall -Werror`. Clang ASan and UBSan each passed a strict helper build, the
focused fixture 20/20, the heavy concurrent stress 10/10, and a mixed
length/JIT-read/weak-finalizer/remote-GC run. The normal default build was then
restored. This evidence is deliberately Linux/x64-only; the complete macOS and
Windows pass remains deferred until b1.2.1 is otherwise release-ready.

## Remaining work

This tranche does not eliminate every ordinary length wait. On a generated
retry, interpreter replay can still enter the retrying `lj_tab_len_rooted()`
path and ultimately `lj_tab_wait_l()`. `table.concat()` also has independent
rooted element-read retry paths beyond its default upper-bound calculation.

Closing the length slice requires a production bounded interpreter
redispatch/help mechanism for paired structural generations, followed by a
mechanical proof that no ordinary length caller reaches a peer-dependent wait.
For structural mutation, the next larger table step remains a persistent,
helpable new-key publication descriptor covering collision-chain `KEYLOCK`
and `FINREG` states, followed by resize/range descriptors. Those designs should
also provide the stable old/successor facts needed for exact interpreter
completion while a resize owner is paused.

A yield-and-redispatch loop is not such a mechanism. During resize the current
vectors remain `RETIRING` until owner-only migration completes, and a forwarded
source value can exist only in the paused owner's C local before successor
installation. Exact completion therefore requires a published resize/migration
descriptor with value-preserving per-slot intent, helpable successor
accounting, and idempotent root publication.
