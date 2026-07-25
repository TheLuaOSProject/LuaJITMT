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
- a nonthrowing `CALLS` to `lj_tab_len_rooted_try()`; and
- a guard that side exits when the result is `LJ_TAB_LEN_RETRY`.

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
- balanced SMR, lease, root-descriptor, and dynamic-root-anchor accounting,
  with no table wait-counter movement and no helper allocation.

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

A preliminary single-run active-MT microbenchmark measured roughly 64 ns per
`#table`, versus roughly 2.8 ns on the pre-MT stock lowering (about 23x). That
is an absolute bounded cost, not performance parity, and it remains explicit
optimization debt. The result is not a release benchmark or a substitute for
the required reproducible three-sample performance gate.

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
also provide the stable generation facts needed to replace the current
active-MT helper cost with cheaper validated fast paths.
