# Binary scalar math helpers

Matched ordinary probes at the sparse/FFI integration show that `math.atan2`,
`math.pow` and `math.ldexp` return correct interpreted results on ARM64 but
publish no selected native loop. The x64 control publishes each loop and
observes eight selected exits for eight calls, both before and after full GC.
Logs and probe source are in
`/Users/frityet/Scratch/ljmt-deep-callm-integration-20261029`:
`binary-math-arm64-before.log`, `binary-math-x64.log` and
`binary-math-probe.lua`.

The existing ARM64 backend already emits these calls. The change adds exact
two-argument builtin preflight, typed numeric operands for POW and LDEXP, and
a two-NUM CARG for atan2. That CARG can only feed the whitelisted binary libm
call, and every future use is checked independently in semantic and allocated
views. It cannot become a Lua snapshot value or a rooted-effect argument.
The existing call-preservation proof now also covers POW and LDEXP, including
later snapshots and loop-carried values. No VM phase lease is released and no
shared-state helper or lock is introduced.

The first ARM64 assertion checkpoint passes the complete scalar capability
contract, including six binary-math native/lifecycle runs, private argument
and type corruptions in both admission passes, and all previous unary/helper
call-preservation tests. `binary-math-second-contract.log` ends OK.
`binary-math-second-probe.log` observes the same eight selected exits per
eight calls before and after GC for all three functions. This build includes
test helpers; the ordinary no-helper follow-up is still pending.

The first experimental run still refused the calls because builtin recorder
preflight had not yet been extended; its failure is superseded by the second
run, with exact argument count and synthetic fast-function identity checks.
Four-profile, special-value and combined-gate validation remain pending.
This checkpoint does not establish general math or full x86_64 parity.

## Expanded four-profile validation

`binary-math-four-profile-final.log` passes the complete scalar contract for
ARM64 and arm64e assertion/release builds. New native cases cover negative
zero, NaN, infinity, underflow and overflow; ordinary edge comparisons also
cover both signed-zero axes and extreme integer exponents. A composition of
all three helpers retains live values across calls and independently rejects
six caller-clobbered allocations per profile. The two admission passes also
reject a CARG reused as a later numeric operand and a malformed future-user
opcode before indexing its IR mode. Existing profile/STOPREQ, native unwind,
GC, flush and all earlier scalar proofs remain in the same passing contract.

Independent read-only review found no concrete safety defect in the typed
operands, exact builtin entry, CARG use restrictions or call-preservation
proof. Its suggested future-use negatives were added to the final matrix.

The matrix exits zero, then rebuilds ordinary assertion ARM64 without test
helpers, profile
`15611ca843cbd59705f3a9296ec65cd4d35f192ccd7ade7a1134e6e37961c1a6`.
That runtime observes eight selected exits per eight calls, both before and
after full GC, for atan2, power and ldexp. The earlier matrix command also
passed, but its terminal output was truncated; the final log above is the
complete retained evidence with the additional future-use negatives.
Combined validation with the latest Lua call changes is still pending.
