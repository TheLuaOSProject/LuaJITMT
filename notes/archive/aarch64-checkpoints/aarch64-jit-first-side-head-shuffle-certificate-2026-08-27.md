# AArch64 first-side head-shuffle certificate (2026-08-27)

## Scope

This checkpoint turns the observed x28-parent to x27-child transfer into an
exact, immutable post-register-allocation certificate. The production side
recorder remains fail-closed, but the assembler-consumption checkpoint now
uses a pure pre-head layout half before parent/map consumption and the full
entry-prefix half after allocation.

## Exact provenance and emitted transfer

`asm_setup_regsp()` now records the exact number of entries returned by
`lj_snap_regspmap()`. For the canonical first side, the certificate requires:

- `stopins == REF_BASE+1` and `orignins == REF_BASE+7`;
- exactly one populated parent-map entry;
- that entry is `REGSP(RID_X28, SPS_NONE)`;
- the child inherited `SLOAD` is allocated to unspilled x27; and
- the private entry prefix contains `MOV x27, x28`, immediately preceded only
  by `BTI J` when branch tracking is enabled.

The machine-code check runs over private assembler output. Backwards emission
places the side-head transfer first at runtime, or second after the optional
BTI landing, so x28 is copied before any generated body instruction can reuse
it. The view's branch-tracking value must equal the target's compile-time
`LJ_ABI_BRANCH_TRACK`; a caller cannot downgrade an arm64e/BTI build to the
non-BTI form. This proves the actual emitted transfer rather than inferring it
from allocator fields.

The pure view carries the parent-map pointer and exact extent instead of a
caller-selected scalar. Production constructs those fields directly from
`ASMState.parentmap` and `ASMState.parentmap_n`. Its compact extent comes from
`J->curfinal->nins`, which owns the trailing NOP during each assembler pass.

## Validation and remaining boundary

The focused fixture executes once against the ordinary ARM64 archive and again
against a directly compiled arm64e/BTI helper object, accepting the exact entry
form selected by each build.
It rejects null or misaligned map/entry pointers, wrong extents and stop points,
every wrong parent register/spill form, missing or malformed BTI, short entry
spans, a correct move displaced behind another instruction, and wrong move
width/opcode/source/destination fields. It also proves that the pre-head half
rejects every semantic/layout/map mutation while deliberately ignoring entry
bytes that have not been emitted yet. The umbrella contract pins the exact
production call and marker ordering while keeping recorder/publication gates
closed.

This closes assembler-side parent-map provenance and emitted head transfer.
Child publication order, authenticated parent-exit publication and real side
retirement remain closed gates.
