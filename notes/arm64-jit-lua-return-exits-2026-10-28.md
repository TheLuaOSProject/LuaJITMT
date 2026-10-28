# Terminal Lua leaf returns on ARM64

## Scope

A fixed pure Lua leaf can return into an ordinary caller-local CSET while its
old-destination type guard retains a child snapshot at RET1. The terminal child
map is now admitted with independently reconstructed return state and an exact
binding to the caller's assignment. Nil, booleans, numbers, lightuserdata and GC
values are covered, including a captured caller local using the existing rooted
cell-store helper.

The change is confined to ARM64 admission and tests. It adds no recorder,
assembler, VM, GC2, FFI, locking or executable-memory write mechanism. General
Lua call/return and x86_64 parity remain incomplete. Full frozen validation and
main-checkout integration are pending at this source checkpoint.

## Source and allocation proofs

The semantic and allocated passes independently replay the normalized pure
leaf prefix through its final RET1. The return can use an earlier formal or
local; it need not be the preceding operation's destination. Both passes
require its exact visible SSA value, exact mapped child slots, required source
reads, frame extent, callee identity, caller arguments and return link.

The retained terminal map must be attached to the exact caller CSET's old-slot
SLOAD type guard. Its destination must precede the CALL window and its source
must be CALL.A. Primitive guards have no materialized value; a P32 guard must
satisfy the existing guard-only cell proof. Internal cell identities cannot
become child return values or snapshot payloads.

The next snapshot must be in the caller and must carry the exact returned SSA
value. For a cell store, it is the CSET pre-helper map's CALL-result slot. For a
raw local assignment, it is the destination slot immediately after CSET or at
the immediately following root FORL exit. The root's certified start address
handles the live FORL/JFORL publication distinction. Other continuations,
missing results, different SSA values and unsupported conversions fail closed.
This output binding was added after a private-clone test changed RET1 to a
different, already-defined child value: a valid restored child alone did not
prove that native execution assigned that same value in the caller.

Dead pure arithmetic need not retain an SSA instruction. Each prefix replay
tracks an unavailable result as zero, which cannot satisfy a mapped slot or
return, including the eventual return of a nonterminal arithmetic snapshot.
The preceding independent complete-body argument/type walk
proves every initial value visible, every source defined, and every arithmetic
operand numeric. Copies, literals and numeric arithmetic preserve that visible
value property, including an old raw-CSET destination whose pure SSA producer
was removed. No zero is treated as an actual value.

A retained child map may be unselectable after DCE. Both passes independently
require an immediate next snapshot and a valid empty or NOP-only IR interval
before that next map. Only then can an absent arithmetic guard be accepted;
the allocated pass also omits physical restoration checks for that unselectable
map. Its source state, required reads and map values still have to match.

A separate allocated CALL-map exemption covers an ignored computed argument
on the backedge. The exact root CALL map's interval to its next snapshot must
contain only NOP or unguarded integer ADD, and its call window must be valid.
Neither instruction can exit; a later guarded consumer selects the next map.
All ordinary map type, source, bounds and flag checks remain active. This
extends the earlier same-reference CALL-to-child map exemption without
relaxing physical restoration requirements for an executable map.

## Focused evidence

`tests/t-arm64-jit-lua-return-exits.c` covers 80 shapes and 320 native
invocations. Twelve TValue categories appear in direct, copy, parameter
assignment, discarded arithmetic, discarded arithmetic overwritten by a visible
value, and captured caller-local layouts. Additional cases cover missing and
extra arguments, an earlier numeric local, a compound return, a retained
constant, a dead dependent arithmetic chain and an ignored computed actual.

Each shape requires one controlled terminal exit and three same-trace reuses,
including one after full GC: 80 terminal exits, 240 reuses and 80 GC reuses.
The oracle result stays rooted on the Lua stack while the candidate executes.
All invocations check base/top/cframe and native/TG cleanup.

The terminal exit uses the existing one-shot POSTADMISSION test pause. While
the owner is paused before generated code, a test worker atomically changes
only the old caller destination to nil, or false when its recorded tag was
nil. It checks the frame's callee and original tag, publishes no new GC pointer,
and releases the owner. Generated code then fails its real old-slot type guard.
The witness requires native entry, the first exit's exact parent/snapshot,
RET1 PC, callee/frame/return link, return-slot presence, interpreter-equivalent
result and subsequent trace reuse. This is a controlled old-tag mismatch, not
a claim of an ordinary input-driven terminal exit. It does not modify trace
bodies or generated code. GC payloads remain independently rooted; post-GC
reuse is not a sole-root survival test.

Private trace/prototype clones exercise both admission passes independently.
They cover return substitution/omission/NORESTORE, RET1 source and count,
continuation PCs, the exact caller CSET source/destination and CALL window,
old-slot guard source/mode/type, frame extent, missing caller output,
unknown/dead returned values, nonnumeric dead arithmetic, NOP replaced by an
executable guard and physical aliases. The current run rejects 1,247 semantic and 1,256 allocated mutations.
All nineteen semantic mutation classes must execute in both passes. Eight allocated conflicts and an additional
allocated guarded-loop-increment mutation are required.

The ordinary Lua companion covers 39 publications, four numeric overflow
ranges and 86 exact-trace reuse checks, including captured locals. It compares
fresh interpreter-only oracle prototypes and makes no native-entry claim.
Both fixtures keep a live MT peer and are included in the four-profile leaf
contract. The channel timeout is in seconds (`threading_timeout_ns`), so the
peer's `recv(60)` is bounded by sixty seconds.

## Counterfactuals and validation

Evidence is retained at:
`/Users/frityet/Scratch/ljmt-lua-return-exits-20261028`.

Nine separately rebuilt counterfactuals passed their expected-failure checks,
targeting semantic/allocated caller-output
binding, old-slot binding, the NOP-only child-map interval, acceptance of dead
pure SSA results, and the allocated unselectable CALL map. Each failed for its intended malformed-clone or missing-publication reason.
The production header was restored byte-for-byte and rebuilt; all seven
C/Lua leaf contracts passed again. Full matrix results will be recorded after
the frozen checkpoint completes. An older compound mutation that changes an
arithmetic destination also verifies that a nonterminal prefix cannot lose
the exact eventual return value.

## Remaining limits and retained observations

This opens terminal return snapshots in the bounded fixed-result pure-leaf
FORL geometry. CALLM/CALLT, varargs, multiple results, nested/general calls,
effectful leaves, general side-trace topology and unrestricted return consumers
remain outside this tranche. The earlier cdata equality-consumer refusal is
separate from returning cdata directly into a caller local, which is covered
here.

The earlier rooted-length native-witness failure after full GC and the prior
callback auto-unwind bootstrap timeout remain unexplained. Their passing
repetitions do not establish a fix. The bootstrap run will retain its normal
deadline; an external watcher only captures stacks if that callback fixture
stalls for more than five seconds.
