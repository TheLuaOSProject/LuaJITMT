# U32 numeric FFI results on Darwin ARM64

## Scope and proof

This increment admits unsigned 32-bit integer CALLXS results converted to Lua
numbers by the existing ARM64 emitter. The ABI must be an unsigned four-byte
integer in a GPR. Each retained result has exactly one unguarded U32-to-NUM
conversion while its native frame is active; the completed-call snapshot at
native leave must restore that exact number to the source CALL's caller slot.
A discarded result may lose its conversion to DCE, but still requires the
completed-call snapshot and exact continuation. Raw U32 carriers are forbidden
in Lua snapshots and cannot escape through other IR consumers.

The semantic and allocated-IR passes derive these facts independently. The
latter additionally requires the result to survive native leave in an aligned
full-width spill or preserved FP location. Snapshot locations now apply the
validated RENAME suffix using the same ordering and snapshot scope as
`lj_snap.c:snap_renameref`. Retained-result loops can keep the old and new
numbers in distinct snapshot-specific FP registers.

The CALLXS grammar also admits the existing exact guarded NUM-to-INT conversion
only while the native frame is closed. Numeric accumulator loops require this
shape after optimization. Its input remains independently validated scalar IR,
and ordinary guard snapshot, allocated-location and native-call liveness checks
still apply. This intentionally composes the existing scalar conversion with
CALLXS; it does not require that every numeric input originate from U32.
No recorder, optimizer, allocator, emitter, shared runtime or x86 code changed.

## Validation

The focused contract is `tools/ci/arm64_jit_u32_contract.sh`. Its runtime and
independent admission fixtures are also registered in the M7 scalar CALLXS
case. The native runtime fixture passes its first ARM64 assertion execution:
ten boundary/high-bit patterns, retained loop results, two distinct native
functions, fixed and variadic signatures, typedef/const results, errno,
forced retained/discarded POSTCALL exits without replay, and full frame cleanup.
A threshold-returning helper drives a real checked NUM-to-INT exit after a
large unsigned result, with exact result and native-call counts.

Initial bring-up distinguished published traces from native execution: an
integer-initialized sum could be recorded with an entry guard that rejected the
already-large sum. Ordinary accumulation tests use a fractional initial value;
the dedicated threshold case exercises the checked narrowing path explicitly.
The two-call fixture loads its second callable before the loop, avoiding the
separate unsupported in-loop global-table lookup shape.

The focused ARM64 assertion contract passes, with 915 observed native calls,
four authentic positive traces and independent malformed-view checks. The
older scalar and pointer CALLXS admission fixtures also pass. The ordinary
ARM64 assertion build passes 26 U32 result smoke cases with exact values, call
counts and published trace metadata. The instrumented C fixture supplies the
native-execution evidence. Initial focused log: `/tmp/ljmt-u32-tenth.log`;
ordinary smoke: `/tmp/ljmt-u32-ordinary-preliminary.log`.

Full focused-profile and compatibility results will be recorded after the
source checkpoint is frozen. No full-port or full-concurrency parity is claimed.

## Remaining boundaries

U32 CALLM producer/consumer chains remain closed: the existing CALLM source
proof does not yet bind a converted U32 number as its producer. U32 enum results,
general 64-bit arithmetic, aggregate/sret results, arbitrary trace shapes and
full sustained multithreading parity remain outside this increment.
