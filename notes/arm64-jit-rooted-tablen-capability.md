# ARM64 rooted table-length JIT boundary

The experimental ARM64 scalar `BC_LOOP` profile now admits one guarded table
`SLOAD` and exact `lj_tab_len_rooted_try` calls rooted from that value through
`TMPREF IN1`. Repeated `TMPREF`s of the same SLOAD are permitted because loop
recording can materialize separate root references for repeated length
operations. Each call must consume one of those exact roots, and each call must
have an immediate `IR_NE` guard against `LJ_TAB_LEN_RETRY`. No other comparison
may consume the helper result, so the sentinel is checked before any later IR
use or snapshot. Call result and table
snapshot values are independently checked by the semantic and post-register-
allocation validators. The post-RA check also requires GPR locations for the
root, call result, and table snapshot source. Since the bounded scalar spill
model only describes 32-bit INT and 64-bit NUM slots, TAB and PGC values in this
profile are required to remain in registers; copied post-RA views with spilled
table SLOAD or TMPREF are rejected.

This profile is deliberately narrow: only a single table SLOAD, only the
rooted try helper, only `TMPREF IN1`, and only the retry guard are accepted.
Other table reads, helper calls, or malformed root/call/guard relationships
remain rejected. No raw table-array read is emitted by this profile.

Native proof on Apple Silicon: a fixture constructs a dense table, activates
MT by spawning and joining a worker, then runs a `while` loop accumulating
`#t`. With `hotloop=1`, the trace contained the guarded table SLOAD, rooted
helper call, retry guard, and `IR_LOOP`, and assembler output reported
`TRACE 1 stop -> loop`; the accumulated result assertion passed. The recorder
produced two helper calls and duplicate TMPREF roots in this loop, which led to
the bounded repeated-call/root shape above.

The focused C contract validates semantic and post-RA positive certificates
against copies of the executed loop trace. Independent mutations reject a wrong
helper, retry sentinel, TMPREF mode, table SLOAD guard, or a missing immediate
retry guard. The semantic copy uses a separately rooted identical function
prototype and rebased snapshot PCs; the published prototype and trace are not
modified.

Lifecycle checks execute the generated helper with active MT, force generation
and root changes at the helper boundary, verify interpreter replay returns the
correct length, and assert no wait calls or leaked read-scope state. The M6
case recognizes the ARM64 helper ID and proves native `BC_LOOP` entry with a
`texit` counter. On ARM64, pre-activation checks assert correct results and no
rooted helper publication; stock `IR_ALEN` is not asserted because the ARM64
assembler deliberately rejects that instruction. The bounded profile covers
`#t`; it does not claim `rawlen(t)` support.

The C lifecycle fixture also pauses at post-admission/pre-native entry and
publishes STOPREQ. It requires the exact loop snapshot exit, completed request
acknowledgement, clean read/root state after error unwind, and a subsequent
native re-entry that calls the rooted helper. Before full collection it restores
the temporary array replacement; then it collects while the function and table
remain rooted in globals and again requires native entry and a rooted-helper
call. The paused STOPREQ and full-GC sequences both passed in four consecutive
focused fixture runs.

The complete dedicated contract also passed after integration with the current
GC2/FFI fixes, on both ARM64 and arm64e/BTI. The registered
`m5_tab_rooted_len_try` and `m6_jit_rooted_table_len` cases passed on that same
integrated source. M5 selects the dedicated ARM64 contract so its fixture and
archive use matching assertion/test-helper layouts, and restores the safe
interpreter profile afterwards. The existing x64 registration remains a
native C fixture.
