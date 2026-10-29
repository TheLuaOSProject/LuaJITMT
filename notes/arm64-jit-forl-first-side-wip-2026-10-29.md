# ARM64 FORL first-side work in progress (2026-10-29)

This isolated branch explores first-side traces selected inside a `BC_FORL`
body. It is not a validated parity capability yet.

## Safety issue and current approach

The source root can be admitted only after the existing `arm64_ir_forl_shape`
semantic and post-RA proofs establish its loop update and integer range guard.
That root-wide certificate is insufficient for a child update: the selected
parent exit may be inside the body before the parent's guard is reached. The
selected snapshot must also bind the child's inherited index, stop, and step
to the exact loop slots.

The current WIP adds an ARM64 experimental recorder check at child FORL index
update construction. For a constant-step integer update with a FORL parent, it
re-emits the existing `rec_for_check(..., init=1)` stop bound before the loop
comparison. The semantic and post-RA child validators require the exact local
`stop <= INT32_MAX-step` guard (or `stop >= INT32_MIN-step` for a negative
step), sourced from the readonly inherited stop SLOAD, after the index ADD and
before the loop comparison. This should keep a failed guard at the interpreter
FORL continuation; that exit mapping still needs a native witness.

The parent snapshot copies the admission token, source interval, generation,
selected map values, and bindings under the existing SMR protocol. The first
side limit remains unchanged. The branch must fail closed if either independent
validator cannot establish the child guard and slot bindings.

The root-entry audit also needs a FORL-specific source-generation proof for an
already-published first child. It now accepts either the existing LOOP form or
the exact FORL/FORI pair: patched JFORL identity, saved FORL instruction,
matching A registers, valid forward FORI target, and a continuation inside the
loop body. This is required before direct native entry can traverse the
root-linked child.

## Current validation status

The focused production contract passed at default repetition counts for smoke,
ARM64 helper, and arm64e helper profiles, including `gc-claim`, `scoped`, and
`full-flush`; the ordinary-range FORL child executed native exits. Log:
`/Users/frityet/Scratch/ljmt-forl-first-side-20261029-preserved/default-contract.log`.
The initial cleanup failure was a fixture bug: the new three-argument FORL
function was called with only two arguments, so its loop was empty and the
post-token hook never fired. Supplying the exact `(start, stop, mode)` tuple
restored the original pending-request assertion and cleanup checks; they pass.

The FORL child copy contract now independently rejects four mutations in both
semantic and post-RA passes: deleting its local stop bound, loosening the bound,
changing the loop-step operand, and moving the bound check after the loop
comparison. The test selects the exact update/stop/step/bound relation from the
copied source and map before applying mutations.

Boundary conformance is checked against a separate explicitly JIT-disabled Lua
oracle. For `start=INT32_MAX-1, stop=INT32_MAX, step=1`, the native candidate
returns the exact oracle result; runtime counters identify an exit from root
trace 12/exit 0, not from attached child 13. Thus this proves whole-loop
boundary behavior through root fallback/interpreter, not execution of the
child-local guard. A separate safe near-maximum case,
`start=INT32_MAX-2, stop=INT32_MAX-1, step=1`, matches the JIT-disabled oracle
and enters child 13 at child exit 2 on both ARM64 and arm64e. This exercises
the child-local bound on a safe edge; it does not trigger the bound failure.
The minimum-bound mirror and an exact child overflow-exit witness remain
incomplete. Do not describe this WIP as a complete FORL parity capability or
integrated.

A previous experiment accepted the child ADD using only the copied parent
admission token and selected map. That path was closed. The present source has
an explicit child guard check. A native child witness exists only for ordinary
in-range values; it does not establish overflow safety.
