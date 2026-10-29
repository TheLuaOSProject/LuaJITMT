# Metacall, aggregate ABI and FORL-side integration

Production source `b549a895b` combines these independently validated changes:

- `49b1f5e6f`: reconcile the recorder's sampled cdata `__call` specialization
  with the interpreter's actual selection. Only the exact recorder owner reads
  or clears pending state. A mismatch aborts the unpublished trace before VM
  argument shifts; the VM still executes its selected function or normal error.
- `74a6f84a3` and its prerequisites: native direct one-float, one-double and
  two-float struct arguments in available Darwin SIMD argument registers.
  Formal Lua arguments and flattened ABI carriers have distinct counts. The
  scalar variadic producer-to-CALLM path retains its suffix argument count.
- `498ba74c9`: native direct integer-struct results of width 1, 2, 4 or 8.
  CNEW retains the declared result CType, XSTORE uses the exact result width,
  and raw result carriers cannot appear as Lua snapshot values.
- `b549a895b` and its prerequisites: one root-linked scalar side from an integer
  FORL root. Both loop directions bind the source step and inherited controls.
  A child-local bound guard's snapshot must retain the old index and resume
  at the loop boundary or a source JMP directly reaching it, after the body.

The recorder repair has deterministic C-to-Lua, Lua-to-C and C-to-nonfunction
tests, with normal results/errors and successful native recording afterward.
ARM64 assertion and x64 assertion/release runs pass, as does the original
ARM64 randomized metacall stress. Its exact pre-fix failing harness is retained.
See [the race analysis](cdata-call-recorder-metacall-race-2026-10-29.md).

The aggregate fixture passes ARM64 and arm64e assertion/release profiles,
including malformed semantic and post-allocation views, native exits, exact
foreign effects, and GC/re-entry. Ordinary ARM64/x64 probes establish native
retained integer and word result loops on both targets; the retained two-float
result still has an x64-only witness. See the
[argument scope](arm64-hfa-scalar-shapes-2026-10-29.md) and
[integer-result evidence](arm64-integer-aggregate-results-2026-10-29.md).

The FORL contract passes ARM64/arm64e smoke/helper repetitions, strict object
compilation and an ordinary selected-side topology probe. Safe near-MIN/MAX
cases run through the child; exact MIN/MAX stop cases exit through the parent
guard and match an interpreted oracle. The child-local guard failure is
structurally checked, not dynamically reached. See the
[FORL-side note](arm64-jit-forl-first-side-2026-10-29.md).

## Combined validation

The full experimental gate is running from `b549a895b`; no complete integrated
pass is recorded yet. Its driver, source identifier and logs are under
`/Users/frityet/Scratch/ljmt-metacall-hfa-forl-integration-20261029/`.
The previous complete gate remains `3e3f3432a`.

This is a port-branch checkpoint, not full x86_64 parity or a main-branch merge.
General Lua calls/returns, recursive/multiple-side topologies and additional
aggregate ABI forms still require work. The separate historical TDUP harness
crash has not been resolved by the metacall repair.
