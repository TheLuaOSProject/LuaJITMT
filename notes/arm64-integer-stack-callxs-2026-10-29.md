# Eight-byte integer aggregate stack arguments (2026-10-29)

Fixed eight-byte integer structs can now be passed by value after the eight
GPR argument slots are occupied. The existing aggregate classifier still
rejects unions, nested/over-aligned structs, bitfields, floating members and
unsupported sizes. Only the existing U64 aggregate carrier is opened on the
stack. Smaller stack aggregates and variadic aggregate signatures stay closed.

The signature planner aligns that carrier to eight bytes and records its
exact stack offset. The generic emitter already uses the same alignment and
full-width store for U64, so it needs no special instruction or ABI change.
Semantic and post-RA validation independently rebuild the argument bank and
stack plan. Both bind the raw XLOAD to the exact guarded CType and original
cdata source, forbid exposing the carrier in Lua snapshots, and retain the
existing XSAVE/native-call lifetime proof. A trailing scalar argument is
checked against the next stack offset (8 in the fixture, with a 12-byte total
argument payload).

## Evidence

An ordinary interpreter/JIT comparison uses eight scalar uint32 arguments,
the integer pair and a trailing uint32 argument, with an attached MT peer.
Against the unchanged pre-feature ordinary bundles at `dbc05bbf3`, x64 has
one selected root with two CALLXS operations and 52 selected-root exits;
ARM64 has no root. Both execute exactly 2,304 primary calls and 64 alternate
calls with equal results. The new ordinary ARM64 and x64 builds both have the
selected root, two CALLXS operations, 52 exits and the same exact effects,
including full-GC reuse and alternate-call fallback. The probe installs only
a texit observer, with no TRACE observer or private test helpers.

The focused aggregate contract passes ARM64/ARM64e assertion/release profiles.
It requires native finish counts and exact-root exits for the new argument,
GC re-entry, alternate-call fallback, and private semantic/post-RA rejection
of malformed carrier widths, source addresses, CType guards, offsets, argument
lists, snapshot exposure and allocated locations. Standalone ABI checks cover
the eight-byte aggregate both with and without a trailing scalar and retain
negative tests for smaller/variadic stack aggregates. Strict Darwin x64
compilation passes for lj_asm.c, lj_crecord.c, lj_ccall.c and both C fixtures.

One early arm64e fixture run failed to observe the new root in its bounded
warmup; its binary/archive and exact test sources are preserved. The same
source subsequently passed in isolation. The fixture now resets the standard
hotloop counters before this independent probe so preceding tests' penalties
do not carry into its warmup. The final four-profile run passes. The original
missing witness was not instrumented to establish its exact cause; this is
not a claimed production liveness fix. A first earlier test failure was a
fixture error: it accidentally queried the HFA-only native counter for an
integer aggregate, and was corrected to use the existing integer counter.

Evidence is `/Users/frityet/Scratch/ljmt-integer-stack-20261029/`, including
`baseline-arm64.log`, `baseline-x64.log`, `four-profile-reset-contract.log`,
`ordinary-validation.log`, and immutable `arm64/` and `x86_64/` runtime,
archive, module and probe logs. `probe.lua` and `fixture.dylib` preserve the
cross-architecture comparison. Source and SHA-256 manifests accompany them.
The integrated full gate is still running at the preceding iterator/HFA-stack
checkpoint; it does not yet cover this integer-stack change.
