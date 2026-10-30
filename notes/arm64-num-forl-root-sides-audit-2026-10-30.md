# Numeric FORL root-side gap audit

## Matched reproduction

The root-side gap is observable with this runner:

```lua
return function(mode, n)
  local sum = .5
  for i = .5, n, .5 do
    if mode == 1 then sum = sum + i * .25 else sum = sum - i * .5 end
  end
  return sum
end
```

The probe was adapted from `bench/jit-topology-probe.lua`. Its oracle loader disables the entire loader chunk before creating the oracle closure; a live MT peer remains active. It runs phases `(1,48)`, `(2,48)`, `(3,48)`, 80 checked calls per phase, a full GC, and one additional call. It observes exits through `texit` and identifies roots from the selected runner's patched bytecode.

On the original ARM64 probe, the numeric `JFORL` root was trace 1. It recorded exits but no linked side, while frozen x86_64 published a selected side. I initially attributed ARM's missing side to an admission refusal, but the diagnostic `jit.attach(trace_observer)` itself suppressed ARM64 first-side publication by installing the TRACE vmevent handler. That run was not valid evidence of an ordinary refusal.

With that observer removed, the same source, independent JIT-off oracle, and live MT peer publish the numeric root side and enter it natively. Current ARM64 phase 2 has `root_ids=1`, `published_root_linked_sides=2`, one root exit and 81 side exits from trace 2. Phase 3 reuses trace 2 for 81 further exits. The exact output and IR dump are in `/Users/frityet/Scratch/ljmt-num-forl-sides-20261030/final-ordinary-peer.log`; the flush-extended script ends with `jit.flush()` and exits successfully.

The focused C side-admission contract now covers both `+0.5` ascending and `-0.5` descending NUM FORL roots. For each, it warms a root, causes a direct root-linked side, compares return values, checks semantic and post-RA copied-view rejection for generic mutations plus wrong typed update operand, wrong ordered-bound direction, broken inherited index source, and a corrupt inherited-index snapshot map, then tests full GC reentry and full flush/reentry. The four-profile result is `/Users/frityet/Scratch/ljmt-num-forl-sides-20261030/final-side-fourprofiles.log`: arm64 and arm64e assertion/release all passed. The contract ends by restoring the ordinary arm64 assertion profile.

## Source boundaries

The central new gate is a source-derived, literal-only NUM recurrence certificate. It takes the parent step from the already-certified numeric root, requires its direction to match the NUM FORL admission, and proves the child ADD and ordered comparison use the inherited index, exact stop slot, and exact step. Semantic and post-RA checks independently validate those operations and the boundary snapshots. Dynamic steps and child sides of numeric child traces remain closed.

The initial native side exposed one cleanup mismatch too: first-side retirement required an INT FORL parent even though numeric sides were admitted and carried a sealed NUM parent certificate. Retirement now accepts only the two NUM FORL admission directions and rechecks the parent’s literal-step proof before detaching the edge. The ordinary native probe, the full-flush check, and the four-profile copied-view contract pass after this change.

## Cross-target compile

The strict x86_64 build completed for the touched sources using `-Wall -Wextra -Werror`. Two unrelated existing warnings prevented a completely unmodified warning-as-error build: unused parameters in `lj_str.c` and an unused-but-set `topofs` in `lj_api.c`. The successful retry suppressed only those two warning classes. Full command output, including the initial attempts, is `/Users/frityet/Scratch/ljmt-num-forl-sides-20261030/strict-x64-build.log`.

After removing unused diagnostic residue, ordinary assertion ARM64 and ordinary
x86_64 each pass all 509 stock source tests and all 509 tests imported from
original v2 bytecode. Matching runtime and JIT module bundles, hashes, and logs
are in `/Users/frityet/Scratch/ljmt-num-forl-sides-20261030/{arm64,x86_64}-ordinary`
and the corresponding `*-stock-{source,bytecode}.log` files. The identical
no-observer numeric FORL runner also passes its independent JIT-off oracle and
live-peer checks on both targets. ARM64 publishes root #1 and side #2, then
reuses side #2 after GC and validates explicit flush teardown; x86_64 publishes
root #1 and side #3.
Phase-two observed 1 root exit plus 81 side exits on ARM64 and 1 root exit plus
5 side exits on x86_64. Exit counts differ, while both runs match the oracle.
Exact topology output is in `arm64-numeric-side-peer.log` and
`x86_64-numeric-side-peer.log`.

The cold FORL boundary snapshot proof reads the KNUM payload from the validated
constant pair in the original IR view. A private fixture constructs the exact
four-slot boundary map from the authenticated NUM FORL refs and requires both
independent validators to accept it; flipping one bit in the step KNUM payload
must make both reject. This is direct coverage of the boundary subproof: the
ordinary captured child trace has no snapshot at that FORL boundary footer.
The fixture passed all four ARM64/arm64e assertion/release profiles in
`/Users/frityet/Scratch/ljmt-num-forl-sides-20261030/step-payload-fourprofiles-final.log`.
