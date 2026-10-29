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

The focused production contract passed one run of smoke, ARM64 helper, and
arm64e helper modes, including `gc-claim`, `scoped`, and `full-flush`; the
ordinary-range FORL child executed native exits. Log:
`/Users/frityet/Scratch/ljmt-forl-first-side-20261029-preserved/contract-followup.log`.
The initial cleanup failure was a fixture bug: the new three-argument FORL
function was called with only two arguments, so its loop was empty and the
post-token hook never fired. Supplying the exact `(start, stop, mode)` tuple
restored the original pending-request assertion and cleanup checks; they pass.

Overflow safety is still unproven. A boundary probe with native
`start=INT32_MAX-1, stop=INT32_MAX, step=1` entered root trace 12 and exited at
root exit 0; it did not produce an exit from attached child 13, so it did not
exercise the child's local range guard. The minimum-bound mirror, an exact
child overflow-exit witness, and ordinary-runtime/concurrency evidence remain
incomplete. Do not describe this WIP as a complete FORL parity capability or
integrated.

A previous experiment accepted the child ADD using only the copied parent
admission token and selected map. That path was closed. The present source has
an explicit child guard check. A native child witness exists only for ordinary
in-range values; it does not establish overflow safety.
