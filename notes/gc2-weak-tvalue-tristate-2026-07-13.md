# GC2 weak TValue tri-state admission (2026-07-13)

## Failure mode

Weak-table clearing used a Boolean TValue validator before inspecting mark and
finalized state. That collapsed a transient lifetime-admission failure into the
same result as a terminal stale/type-mismatched slot. It also released the
observation scope before calling the mark oracle, marking a string, or reading
the finalized flag. An address could therefore be reused between validation
and the semantic decision, and `lj_gc2_ismarked()` returning unknown was treated
as unmarked and irreversibly cleared.

Hash processing compounded the issue by expressing key/value clearing as a
short-circuiting Boolean OR. An already-clearable key could prevent observation
of a retrying value and let the clear proceed. Overflow processing then ignored
table-processing failure and continued into the legacy bridge, allowing weak
completion to report success after partial or unknown work.

## Implemented protocol

The conservative stack admission helper is now a shared scoped TValue
admission primitive. It returns `RETRY`, `STALE`, or `ADMITTED`; `ADMITTED`
transfers the exact small-arena or huge-reader scope to the caller. The existing
authoritative-edge `_known` validator remains separate.

Weak classification maps that admission into three semantic outcomes:

- terminal stale or tag/header mismatch: `CLEAR`;
- transient observation failure, unknown mark status, or failed nested string
  mark under the retained incarnation: `RETRY`;
- non-collectable values, live objects, and strings: `KEEP`.

The exact observation scope remains held through the type decision, string
mark, `lj_gc2_ismarked()` result, and userdata finalized-bit read. The embedded
empty string remains an explicit stable special case.

Array and hash scans return failure to their owner on `RETRY`. Hash scans always
classify both key and value before combining the outcomes, so either retry
vetoes clearing even when the other half is already clearable. Snapshot scan
and clear owners already commit their cursor only after successful table
processing, so the same index remains visible for a bounded later turn.

Overflow clearing now admits every native overflow record and exact table,
propagates any record/table/slot failure, and publishes counters only after the
whole overflow list succeeds. A failed overflow pass returns before walking the
legacy weak bridge. Duplicate partial clearing remains safe through the existing
keyed weak-slot CAS, but it can no longer certify completion or advance bridge
work.

## Deterministic coverage

`LJ_GC2_TEST_HELPERS` fixtures inject one failure into the shared TValue
admission point and verify that the first weak drain leaves both slot and clear
cursor unchanged. The retry is covered for:

- an already-marked weak table value, which is retained on replay;
- a weak string value, which is marked and retained on replay;
- an unmarked weak table value, which is cleared on replay;
- a weak-key/weak-value hash entry whose clearable key must not hide the value
  retry;
- the overflow path, where a retrying overflow value prevents both overflow
  success and clearing of a supplied bridge table until the next call.

Validation completed in the isolated implementation worktree:

- helper-enabled build: pass;
- focused weak-only `t-gc2-traverse`: pass;
- full `t-gc2-traverse`: pass;
- `src/luajit tools/test.lua m8_weak`: pass in its default and paranoia/Werror
  matrices;
- clean production `make -j2`: pass.
