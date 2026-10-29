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
