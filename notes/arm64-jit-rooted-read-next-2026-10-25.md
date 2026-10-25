# ARM64 rooted numeric table read (2026-10-25)

The experimental ARM64 recorder emits `lj_tab_getscalar_rooted_try` for
numeric shared-table reads. The new admission profile permits fixed key `1`
from one guarded table SLOAD in a `BC_LOOP` root. It requires exactly two
helper sequences: one before LOOP and one after XPOLL. Other keys, result
types and loop geometries do not gain admission from this change.

Each sequence has an IN1|OUT1 table/output TMPREF and an IN2 key TMPREF, the
exact CARG tree, the no-wait helper CALLS, a guarded `NE result, 0`, and an
offset-zero guarded NUM VLOAD. A refusal leaves the output unchanged and exits
before VLOAD. A successful scalar hit followed by a NUM type failure uses a
new snapshot at the VLOAD boundary, so it does not replay the helper.

Semantic and post-RA selectors independently check the graph, constants,
loop/poll order, and snapshots. Constant membership walks skip KNUM payloads;
root pointers require distinct GPRs and cannot spill. Existing scalar
allocation, snapshot and PHI checks still apply. Non-numeric recorder paths
retain their original TValue helper and snapshot behavior, including the
cdata `__call` path. No raw table load/store capability is opened.

The dedicated contract passes native ARM64 and arm64e/BTI builds with matching
assertion, TRACE, GC2, and TAB helper flags. It executes independent semantic
and post-RA corruptions (bounds, constant payloads, helper/root/guard graph,
snapshots, location aliases/spills, and raw loads), plus native lifecycle
tests. Snapshot negatives restore and reaccept the pristine trace before
testing the next mutation. Copies of published IR use only `[nk, nins)`.

Lifecycle coverage includes a live peer, concurrent value replacement and
array resize, full collection and reentry, a deterministic failed root
confirmation, STOPREQ at native admission, cleanup, and native resumption.
Forced BOOL, nil, and pre-rooted GC-valued results each require exactly one
trace-leased helper call and one exit from the original trace. BOOL exits at
the VLOAD snapshot; nil and GC-valued refusals exit at the earlier status
guard. All three produce the interpreter's arithmetic error with clean root
and read state.

`tests/t-arm64-jit-rooted-read-loop.lua` proves the exact helper/root graph,
native `texit` execution, and observation of a changed numeric value while a
peer remains attached. Both the C admission fixture and the registered
`m6_jit_rooted_read_ir` case reuse that program. The complete experimental gate
enrolls `tools/ci/arm64_jit_rooted_read_contract.sh`.

This remains a bounded experimental capability. Arbitrary keys, GC-valued
native results, metamethod reads, iteration, stores, and other heap effects
remain outside this admission profile.
