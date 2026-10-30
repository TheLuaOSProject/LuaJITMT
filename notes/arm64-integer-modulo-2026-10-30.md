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
