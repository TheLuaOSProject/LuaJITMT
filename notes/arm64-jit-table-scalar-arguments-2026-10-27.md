# ARM64 scalar table arguments to CALLXS

This increment admits guarded INT, NUM, TRUE and FALSE results from bounded
rooted table reads inside existing fixed-signature scalar FORL CALLXS roots.
The results can supply the existing integer, double and float ABI carriers.
The bounded scalar helper retains its primitive-key restriction; the generic
rooted hit helper can also return a scalar through the existing GC-key lease
protocol. Nested table receivers retain their prior independent proofs.

## Admission and scope

Both semantic and allocated passes independently certify the helper, scratch
roots, success guard, exact guarded VLOAD, source TGETS/TGETV/TGETB, retry PC,
source map and following result snapshot. For booleans, the guarded VLOAD
remains the type witness while the result map uses the recorder's canonical
REF_TRUE or REF_FALSE. A mismatched canonical primitive is rejected.

After a certified scalar read, subsequent CALLXS operations also need the
existing exact XSAVE argument-source proof. This binds a numeric conversion
or folded boolean KINT to the actual saved Lua argument, including an
ordinary restorable slot and complete frame span. The semantic pass and
allocated pass derive this separately. Development mutation tests caught this
missing requirement in the initial draft; merely admitting the scalar VLOAD
would not have activated that proof for an SLOAD callable.

The existing CType, register/stack ABI, carrier conversion, live GC root,
physical location, native-frame and continuation checks remain required.
No emitter, recorder, table helper, GC2 runtime, atomic authority, native frame
or x64 production code changes. This opens composition of existing operations;
it does not add a new number-to-integer conversion or adjacent ABI classes.

## Focused evidence

At baseline `47c6e98b`, seven ordinary probes interpret correctly but reject
assembly at CALLXS: integer, double, float, true, false, a table-identity key
and a nested receiver. The initial draft publishes all seven with unchanged
results. Logs: `/tmp/ljmt-table-scalar-red.log`,
`/tmp/ljmt-table-scalar-red-ir.log` and
`/tmp/ljmt-table-scalar-green-ir.log`. Frozen validation below must be used for
the final source rather than these development binaries.

The native runtime fixture covers twelve executions with an active peer:

- TGETS integer, TGETV double, TGETB float, true and false converted to C int,
  plus a table-identity key returning a double.
- A second table read and native call after consuming the first call's result.
- Same-type peer replacement reusing the exact compiled trace.
- Nine integer arguments, including Darwin's stack argument; eleven mixed
  arguments with nine GPR and two FPR carriers.
- An INT-to-TRUE peer change exits at the original TGETS before any native
  finish, then performs exactly two interpreter calls.
- A forced POSTCALL exit resumes after CALL and preserves exactly one foreign
  effect per iteration.

It observes actual native-frame finishes, callable and trace identity, owner,
pins, synchronized active frame, errno, exact C counters and cleared state.
The helper library handle is retained by the configure closure.

The independent admission fixture starts with eight authentic traces. It
accepts each in both passes and rejects 156 semantic and 196 allocated
mutations. Cases include wrong helper/key domain, status guard, scratch
modes, CARG roots, VLOAD identity/tag/offset, source and result maps/PCs,
canonical booleans, exact XSAVE source/restore/span, conversion/ABI type,
register bank, scratch aliases and spill bounds. A half-slot spill is a
negative only for 64-bit NUM, since odd spill slots are valid for 32-bit INT.

All 16 fixtures in the extended table-call contract pass ARM64 assertions:
`/tmp/ljmt-table-scalar-focused.log`. The new tests are also registered in the
broader M7 scalar CALLXS suite. The focused run restores an ordinary ARM64
assertion runtime. Four-profile, umbrella and stock validation are pending
at this development checkpoint.

## Remaining work

This is an incremental experimental JIT opening, not completion of the ARM64
port. Fractional NUM-to-C-int narrowing, general table PHIs, arbitrary Lua
calls/metamethod effects and other uncertified operations remain closed.

A concrete next gap is keeping a scalar CALLXS result live across a second
CALLXS before consuming it:

```lua
local a = fn(t.value)
local b = fn(t.value)
sum = sum + a
sum = sum + b
```

The ordinary probe `/tmp/ljmt-table-scalar-held.lua` returns 448 correctly but
rejects assembly with IR_RENAME; the accumulated-before-second-call variant
compiles. Log: `/tmp/ljmt-table-scalar-held.log`. IR_RENAME is also the generic
post-RA rejection marker, so it does not identify the failed predicate. The
first scalar call result is genuinely live across the second call in this
trace. Snapshot-specific allocation/rename handling is an audit candidate;
no fix or exact rejection diagnosis is claimed here. The runtime fixture's
read-after-call case does not cover that held-result shape.
