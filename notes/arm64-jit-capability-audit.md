# Next ARM64 JIT capability boundary

2026-10-22. The current integer scalar gate already admits arbitrary checked
INT arithmetic within its single-frame self-linked LOOP geometry. Most NUM
restrictions encode particular accumulator examples: bytecode length, exact
IR references, fixed constant profiles, no intermediates and no spills.
These should become instruction/type/provenance and allocator rules rather
than additional trace arrays. The full-pressure experiment found and fixed a
real PHI spill-copy boundary bug; see `arm64-jit-phi-spill.md`.

## Minimal implementation sequence

1. Replace the numeric grammar selectors in `lj_asm_arm64_admit.h` with generic
   scalar predicates for checked INT arithmetic, NUM ADD/SUB/MUL/DIV, INT→NUM,
   checked NUM→INT, numeric comparisons and same-type PHIs. Validate constants
   by IR representation, not literal value. Retain the existing root bytecode
   geometry, single frame, self-link, and full XPOLL immediately after LOOP.
   Keep FORL index/stop/step inheritance and overflow requirements explicit;
   they express loop semantics, not a particular arithmetic body.
2. Independently validate the immutable post-RA view: allowed semantic IR,
   reference ordering, GPR/FPR class, 32-bit versus 64-bit spills, even aligned
   two-slot values, stack capacity, legal rename scopes, and all snapshot
   sources after rename resolution. Permit absent allocation only for unused
   results; every snapshot source needs a real register/spill or constant.
   Preserve packed snapshot maps, root frame sentinel, bytecode PC/base
   geometry, PHI marking and guard/XPOLL snapshot invariants. Do not limit
   registers, IR numbers or frame sizes to examples.
3. Add one rooted-read capability, using the recorder's existing active-MT
   `rec_idx_mt_shared_load()` output. Admit `TMPREF(tab,IN1|OUT1)`,
   `TMPREF(key,IN2)`, its bounded CARG chain, guarded CALLS with the exact
   `lj_tab_gettv_rooted` ID and `(parent,key,parent)` alias relationship, and
   a guarded scalar VLOAD at offset zero from that result. Track scratch
   provenance so a VLOAD cannot consume a result after another scratch
   overwrite. This is an ABI rule, independent of the trace's IR numbering.
   Begin with table/key inputs from checked SLOADs or retained constants and
   INT/NUM results; no GC PHIs or arbitrary GC-valued helper outputs yet.

Keep raw AREF/HREF/UREF/FLOAD/ALOAD/HLOAD/ULOAD, stores, allocation, arbitrary
helper IDs, CALLXS, stitches and wider side traces outside this new capability.
Existing separately proved CALLXS and first-side paths can stay independent.
The helper-only reader does not require opening the x64-specific recorder
macro; active-MT point reads already use the generic rooted path.

## Root and safepoint facts to prove

`lj_tab_gettv_rooted()` leases exact table/key/result incarnations, releases
leases and SMR before retry waits, and release-publishes the result into the
TG scratch root. Its recorder path also inserts a post-helper snapshot before
the VLOAD guard. `lj_gc2.c`'s native-entry gate retains GC bodies while
`jit_base` remains published; ordinary CALLS retain that publication. TG
scratch roots are enumerated during owner acknowledgement. Therefore a
whole-frame XSAVE before every read is not established as necessary.

For the first read capability, unchanged source stack slots/trace constants
retain all GC inputs and only scalar values emerge into registers. Validation
must still exercise helper retry, STOPREQ unwinding, poll/profile requests,
GC gate closure, trace flush, result-type mismatch and root restoration.
Admitting newly created GC values or GC recurrences later needs a distinct
root-retention proof beyond two scratch slots.

## Evidence and meaningful tests

A disposable arm64e prototype used the existing assembler unchanged except
for the spill-copy fix. Mixed NUM intermediates `(x+a)*b/2-.25`, a NUM
accumulator in integer FORL, checked INT chains, and an active-MT `t.value`
loop all matched interpreted output. A fresh repeat call for each observed
one native root publication and one native exit. The rooted reader spilled
two INT values into fixed spill slots. These probes establish feasible
lowering, not a production admission certificate.

Production tests should use generated families with varying constants,
operand order, loop lengths, register pressure and snapshot placement, and
compare values and actual native exit counters rather than assert complete
IR arrays. Include mixed register classes, 32/64-bit spills, rename/PHI
cycles, arithmetic overflow, NaN/type exits, and pre/post-activation runs.
Force a rooted helper miss/retry, mutate result type, and hold GC-only inputs
across a concurrent cycle/resize. Corrupt each semantic and post-RA invariant
independently and verify publication/native entry rejects it. Negative raw
heap, allocation and unrelated-call programs must remain unadmitted.
