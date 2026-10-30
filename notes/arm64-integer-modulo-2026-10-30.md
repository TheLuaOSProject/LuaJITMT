# ARM64 integer modulo

An ordinary live-peer probe at `68c122f2a` produced a native x64 loop for
`sum=sum+(x+i)%7`, but no ARM64 loop. ARM64 diagnostics refused IR_MOD.
The existing common assembler already calls the pure ARM64 `lj_vm_modi`
leaf helper, so this change leaves the emitter and helper unchanged.

The semantic and allocated passes admit only INT results and INT-proven
operands in root traces. A constant divisor must be a nonzero KINT; a dynamic
divisor needs a preceding guarded INT NE against exact KINT zero on the same
SSA value. The allocated pass also checks ordinary and snapshot values across
the helper call with the existing call-survival proof. General side admission
and I64/U64 modulo stay closed. No lock or shared-state effect is introduced.

Private-trace mutations independently exercise NUM result/operands, guard
flags, forward references, zero divisor, missing/reversed/unrelated guard,
nonzero guard constant, and a live divisor moved to volatile X0 without a
spill. The ordinary companion requires a selected loop with native code,
ARM64 IR_MOD, repeated root exits, and unchanged code across full GC. Constant
and dynamic divisors run in FORL and while loops. Signed INT32 limits,
negative divisors, overflow, zero, nonfinite/fractional inputs, string coercion
and invalid types are compared with a separately disabled interpreter oracle.

Initial assertion profile passes the scalar corruption fixture and existing
NUM-string publication fixture. The restored ordinary ARM64 build passes all
four modulo rows. Evidence is in
`/Users/frityet/Scratch/ljmt-next-numeric-primitive-inventory-20261030/`:
`mod-scalar-development-profile-v3.log` and
`ordinary-modulo-development-v3.log`. Earlier failed fixture attempts are
retained: one required constants that the optimizer had removed, and one
mutated the allocated copy while checking the separate semantic copy. The
corrected tests mutate the exact copy consumed by each independent validator.

The separate result-local/result-pair probe still refuses an allocated
RENAME shape. It is retained as `ordinary-modulo-result-pair.lua` and
`result-pair-diagnostic.log`; the accumulator slice does not claim that gap
closed. All four profiles and ordinary x64 validation remain pending at this
initial source checkpoint. The new focused contract runs those four ARM
profiles and is included in the complete gate.

The first focused four-profile wrapper stopped after its passing assertion
scalar fixture because `lj_ci_run_fixture` requires a compiled fixture's
`.profile` sidecar; the ordinary Lua runtime has no such sidecar. The wrapper
now uses a bounded direct runtime invocation after the profile-matched build.
This was runner setup, before the ordinary probe executed, not a runtime
failure. The original log remains in `modulo-final/four-profile.log`.

## Completed focused validation

Frozen source `4bd2d27f9` passes the complete four-profile modulo contract:
ARM64 and arm64e assertion/release, including independent malformed-proof
checks and all four ordinary modulo rows per profile. It restores ordinary
ARM64 at profile
`f42266bad2586fcc559a3784b55e2474ef7e4d4eb0c75cd5596e1a377cac3d97`.

Fresh ordinary ARM64 and strict x86_64 (`CFLAGS=-Werror`) builds each pass
both 509-case stock source/bytecode corpora, all four live-peer modulo rows,
and the 15-row next-primitive inventory. ARM64 selected roots contain IR_MOD;
the default single-number x64 uses its NUM arithmetic graph. Both match the
interpreter across the signed-limit, zero-divisor, overflow/coercion and GC
cases. Exit counts differ and are not performance measurements.

All logs, matching ordinary executables/archives/JIT modules and hashes are
in `modulo-final-v2/` under the evidence directory above. Final explicit
ordinary ARM64 restoration is profile
`538243f3090ccf12d84e157ae88411395f274a2e98ed4205701b237520d64e30`.
This supersedes the initial pending-validation paragraph. The result-local
allocation gap is still open; no complete-gate or full-parity claim is made.
