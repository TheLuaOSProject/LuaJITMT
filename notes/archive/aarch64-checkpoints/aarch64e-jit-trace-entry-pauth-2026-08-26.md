# ARM64e authenticated JIT trace-entry negative contract (2026-08-26)

## Scope

This tranche pins one narrow security boundary after opening the constrained
integer `BC_LOOP` root on macOS ARM64e: the VM must enter `GCtrace.mcode` only
when `GCtrace.mcauth` carries an IA signature made with that exact `GCtrace *`
as its discriminator.

It does not broaden recorder or entry admission. Side traces, stitched traces,
`JFUNCF`, spills, calls, and all IR outside the existing spill-free integer
loop remain closed.

## Typed atomic publication read

`trace_mcauth_acq()` in `src/lj_jit.h` now loads the `ASMFunction` field with
`la_loadfunc_acq(&T->mcauth)`. This keeps the acquire operation typed as a
function-pointer load. In particular, it does not round-trip an authenticated
function pointer through `void *`, so its PAC representation is preserved
explicitly across the publication read.

The production entry helper still performs two independent acquire reads. It
strips the loaded value only for raw `mcode` identity/range comparison, returns
the original signed bits, and the ARM64e VM transfers with `BRAA target, trace`.
The trace identity in `x0` is therefore the required authentication modifier.

## Executable negative proof

`tests/t-arm64e-jit-trace-pauth.c` has one supervisor and four fresh-process
modes:

- `control` records the exact admitted loop and executes a second JLOOP entry
  with the published `IA/mcauth + GCtrace *` pointer. It must return `210`,
  preserve the C frame and TG interpreter state, publish exactly one native
  lease, and exit through snapshot 8.
- `raw` strips the valid entry signature and publishes the raw `mcode` bits.
- `ia-zero` signs the same raw `mcode` with IA and discriminator zero.
- `wrong-trace` signs it with IA and the address of a different `GCtrace`
  object as discriminator.

Every child first records a valid baseline. Before and after the one-field
mutation it validates the exact source bytecode geometry, JLOOP patch, trace
topology, retirement/admission gates, BTI landing, machine-code bounds, no
side trace, exact integer IR including both allocator `RENAME`s and zero
spills, and all nine snapshot references. All three invalid pointers still
strip to the exact published raw `mcode` address, so they pass the helper's
ordinary address/body predicates. Their only invalid property is the signature
expected by `BRAA x1, x0`.

The supervisor uses `posix_spawn()` and `waitpid()` and requires each negative
child to be terminated by `SIGBUS`, Darwin's delivery for the observed
`EXC_ARM_PAC_FAIL`. It deliberately does not accept `SIGABRT`, so an assertion
cannot masquerade as pointer-authentication enforcement. It also does not use
the shell's `128 + signal` convention, which would be ambiguous with an
ordinary exit status. A private pipe carries a readiness byte only after the
child has completed its valid baseline, installed the mutation, and revalidated
the exact body; the supervisor rejects any signal that arrives before that
byte. If a negative JLOOP attempt returns for any reason, the child restores
the original signed field before inspecting counters or exiting; a later
shutdown cannot create a false positive.

`tools/ci/arm64e_jit_trace_pauth_contract.sh` builds a real `arm64e` + BTI VM,
checks that its JLOOP contains `BRAA x1, x0`, runs the supervisor, and holds the
shared test-runner lock until it has restored the checkout to the ordinary
ARM64 experimental build. Its EXIT/signal cleanup performs that restoration
even when compilation or execution fails.

## Boundary of the result

This is an executable negative proof for one authenticated root-loop entry on
this macOS/ARM64e system. It proves neither arbitrary trace shapes nor a
concurrent attacker model, and it does not claim that `SIGBUS` is a portable
signal mapping on non-Darwin systems. The test intentionally requires the
current Darwin behavior and otherwise skips outside native macOS ARM64.
