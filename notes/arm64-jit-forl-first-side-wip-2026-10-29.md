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

## Current validation status

The production helper build reached the `expect_post_token_request_cleanup`
assertion in the focused contract and failed there. No final four-profile result
exists. The native child exit, INT32_MAX/step-1 boundary, minimum-bound mirror,
malformed map/physical mutations, GC/retirement/reuse, and ordinary-runtime
witness are therefore incomplete. Do not describe this WIP as passing or
integrated.

A previous experiment accepted the child ADD using only the copied parent
admission token and selected map. That path was closed. The present source has
an explicit child guard check; a side publication observed before the latest
fixture rewrite did not establish native execution or overflow safety.
