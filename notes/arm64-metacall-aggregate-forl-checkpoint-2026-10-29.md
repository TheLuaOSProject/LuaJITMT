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

Ordinary assertion builds of the exact integrated source `b549a895b` pass
all 509 stock source cases and all 509 imported stock-bytecode cases on both
ARM64 and x86_64. The snapshot-PC fixture, vararg companion, twelve-case
topology probe, twenty-workload inventory and eight-case aggregate probe also
pass on both targets. The inventory has 260 result checks and selected-root
exit witnesses in 18/20 rows on each target. The ARM64 topology probe witnesses
root 1 and root-linked side 2 in the integer-FORL alternate branch. The
aggregate probe witnesses the two-float argument and retained integer/word
results natively on ARM64; the retained two-float result remains x64-only.
Strict ARM64/arm64e assertion/GDBJIT compilation of the combined trace and
assembler units passes with warnings treated as errors.

The unchanged pre-fix remote-flush Lua test, extracted from `1487ee15b`, also
passes against both ordinary integrated runtimes. On ARM64 this includes the
original 20,000-iteration metacall replacement stress; x64 follows its existing
boxed/bool/sret remote-flush path. The dedicated deterministic metacall test
separately supplies x64 race coverage, as described above.

The ordinary driver, source ID, logs, universal fixture library, immutable
runtime/archive/module bundles and SHA-256 hashes are in the `ordinary/`
subdirectory of the combined evidence directory below. The independent
validation checkout was the existing `arm64-math` worktree, detached at the
tested commit, so the full gate's profile builds were not interrupted.

The full experimental gate completed successfully from `b549a895b`. Its final
success marker is at line 19515 of `experimental.log`; all four ARM64/arm64e
assertion/release M7 FFI runs passed, including the deterministic cdata-call
race harness and aggregate contract. The gate also passed its FORL-side
publication/retirement, GC, source/import, and native coverage checks.
The saved driver uses `set -eu`; the process has finished and the final marker
follows its gate profile restoration. Its source identifier, driver and
complete logs are under
`/Users/frityet/Scratch/ljmt-metacall-hfa-forl-integration-20261029/`.
This supersedes `3e3f3432a` as the latest complete port-branch gate.

This is a port-branch checkpoint, not full x86_64 parity or a main-branch merge.
General Lua calls/returns, recursive/multiple-side topologies and additional
aggregate ABI forms still require work. The separate historical TDUP harness
crash has not been resolved by the metacall repair.
