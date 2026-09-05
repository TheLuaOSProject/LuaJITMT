# XSAVE staging fixture admission, 2026-09-05

The XSAVE staging failure was a test scheduling error. The fixture tried to
record its final allocation-free producer during SWEEP, when new recording is
deliberately refused. Earlier allocating traces existed and ran, but exited
before their XSAVE stores. The unchanged staging poison correctly failed the
test; merely observing native execution elsewhere was insufficient.

The fixture now completes collection, checks IDLE with no active native trace,
and resets all three staging words immediately before warming that producer.
Every original shape, snapshot, owner/frame lifecycle and GC certification
assertion remains. The final-marker comment now identifies `math.abs` inside
`stage`, which uses the existing test-only XSAVE injection.

The original baseline diagnostic records 403 recording attempts for `stage`,
all during SWEEP, and a hardware watchpoint confirms untouched poison despite
228 native exits. The corrected diagnostic observes the actual generated root
store in trace 156, followed by base slot 9 and extent 12. Its final IR has one
loop, two XSAVE instructions and no object allocation. The allocating-trace
checks retain their separate static materialization coverage.

The final fixture passes on exact `b4e26564`, that baseline with the callback
stack repair, and the current poll/callback source. Clang ASan/LSan passes on
the isolated callback source with `detect_leaks=1:abort_on_error=1`. The
canonical `m6_jit_xsave` case passes on the current source. Disabling only the
final producer fails the original poison assertion; removing collection fails
the new IDLE precondition. Both negative controls are retained.

All 224 runtime/generator inputs used for the current-source validation match
`f43a9f24`. The four existing shape/lifecycle checker bodies are independently
verified byte-identical. No production source changes. Final fixture SHA-256:
`ad895d2f2757644ffe097121af528ddeb1fbac9c66c950642c39722175fb282d`.

[The detailed diagnosis](evidence/jit-xsave-staging-fixture-2026-09-05/review.md)
and [verified evidence manifest](evidence/jit-xsave-staging-fixture-2026-09-05/artifact-manifest.json)
preserve 58 artifacts: exact commands and source identities, baseline failures,
both controls, generated-store observations and passing lifecycle checks.
This closes the XSAVE fixture gate recorded with the callback repair. The
separate remote-flush readiness failure remains under investigation. Linux x64
only; no performance or release claim.
