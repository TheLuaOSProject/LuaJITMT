# Three/four-member HFA result integration

Production checkpoint `b1d19b91b` integrates the three/four-lane float and double
result groups, their independent allocated-state checks, and ordinary portable
aggregate comparisons. The integration preserves the existing scalar-double
fixture counter at index 14 and shifts the new aggregate counters to 15–19.

The ordinary ARM64 and x64/Rosetta matrix passes: 509 stock source cases and
509 imported-bytecode cases on each target, topology/inventory comparisons,
portable aggregate value/effect checks, and strict ARM64/arm64e GDBJIT trace
and assembler compilation. Evidence and immutable runtime/archive/DSO hashes
are in `/Users/frityet/Scratch/ljmt-hfa-multilane-integrated-20261029/ordinary/`.

The integrated gate passed the earlier contracts through rooted cell operations
and the ARM64 callback profile, then timed out after 30 seconds in the ARM64e
`t-profile-blocked-tg.lua` callback/profile case. The original failure is kept
in `full-gate.log`. The contract's exit cleanup automatically restored the
ordinary runtime; the exact failed executable was not preserved. The unchanged
source and test scripts are recorded in `callback-timeout/`.

A complete focused callback contract rerun passes all ARM64/ARM64e/x64 assertion
and release profiles plus no-JIT profiling (`callback-repeat.log`). This does
not explain or fix the original timeout. No timeout extension or assertion
relaxation was made. A continuation starting after that contract is running
against the unchanged production checkpoint; its source and log are retained
as `resume-source.txt` and `resume-gate.log`. This checkpoint must not be called
an uninterrupted gate pass or a resolution of profiling liveness.
