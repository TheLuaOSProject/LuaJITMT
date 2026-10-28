# Bind nonterminal Lua leaf results to their caller

The new primitive-local overflow tests exposed a source-proof gap. For
`local a=x+3; local flag=false; flag=a*2; return flag`, changing the final
assignment's source to `x` in a private prototype clone could pass admission
for a nonterminal child snapshot. The prefix proof only required a visible
return value. Earlier arithmetic-tail tests happened to reject analogous
changes because the resulting arithmetic IR could not be found. The existing
terminal RET1 proof already bound its result to the caller; the missing check
was in the nonterminal case. This was a malformed-source acceptance finding,
not an observed wrong answer from ordinary generated code.

Both independent passes now retain the complete child prefix's returned SSA
reference. A later terminal map can supply the output binding only with the
same function, frame/return link, and exact returned-value entry; that terminal
map is independently checked by the full snapshot pass. Otherwise each pass
replays the pure caller continuation from its exact CALL snapshot to the first
root map in the same loop iteration. Copies, primitive/numeric constants and
numeric arithmetic propagate the returned value to the caller's saved locals.
Every mapped result-derived slot must match exactly; derived slots below the
map extent cannot be silently omitted. At least one output witness is required.
Source operations, helpers or topology beyond this pure continuation remain
fail-closed.

Unchanged stack locals may first be loaded after the CALL. A missing incoming
value is resolved only through its guarded, visible-value SLOAD in that caller
interval, before any source write to the slot. CGET/CSET copies reject internal
cell values. A raw CSET destination needs a visible old value or its exact-slot
raw tag guard before the output map, which may occur after the leaf returns.
The root FORL/JFORL publication distinction remains separately validated by
the enclosing root certificate. No runtime, emitter, locking, GC root, or
executable-memory mechanism changes.

The compound fixture now covers primitive locals both before an overflowing
operation and after an earlier overflow resume PC. The exact false local must
be present when live as a later CSET's old destination. Earlier exits verify
the future definition and overwrite. A legacy mutation of the initial CGET is
applied only when that instruction is actually CGET; setting an existing
`KPRI false` selector to false would be a no-op. New independent malformed
views substitute, omit or suppress restoration of the caller's saved result.

All four ARM64/arm64e assertion/release leaf profiles pass on this source:

- Compound fixture: 14 cases, 96 native invocations, 34 arithmetic child exits,
  62 same-trace reuses, 14 full-GC reuses, 1,782 semantic negatives and 1,850
  allocated negatives, including 22 physical aliases.
- Return fixture: 84 cases, 336 native invocations, 84 controlled terminal
  exits, 252 reuses, 84 full-GC reuses, 1,320 semantic negatives and 1,329
  allocated negatives.
- All earlier leaf, child, argument, sparse-map and normalized-argument
  contracts, including the two-accumulator caller and dead child arithmetic,
  pass in the same profiles.

Evidence: `/Users/frityet/Scratch/ljmt-leaf-primitives-20261028/`.
`result-binding-four-profile-5.log` is the final four-profile run. Earlier
logs retain the original malformed-source acceptance and intermediate test
and proof failures. `result-binding-focused-4.log` also records a passing
rooted-length fixture with the extended failure diagnostics. The initial
full experimental gate stopped at the old numeric-only test assertion; a
complete gate on the final source subsequently passes. See the
[integrated checkpoint](arm64-parity-checkpoint-2026-10-28.md).

Two separate counterfactual builds from `29e3198a` disable only the new
semantic or post-RA result-binding helper. Each fails at the intended new
caller-result substitution (`mutation=38`, snapshot 2) in its own pass.
The archive source is restored after both runs. Evidence is under
`/Users/frityet/Scratch/ljmt-caller-result-counterfactual-20261028/`, including
`results.log`, `semantic.log` and `postra.log`.

This closes a return-binding gap and expands bounded primitive-leaf coverage.
General Lua calls, branches, trace topology and full x86_64 parity remain open.
