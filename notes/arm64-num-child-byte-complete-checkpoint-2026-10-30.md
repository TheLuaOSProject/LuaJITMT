# Complete NUM loop, child-call and byte-range checkpoint

The complete fail-closed gate passes at frozen source `89226814d`. This adds
NUM FORL root direction support, guarded FUNCF producer-child overflow exits,
integer-literal CALLM replay, FFI integer accumulator PHIs, and default/fixed
positive string.byte ranges to the previous complete `6c46f9bff` checkpoint.
No runtime locks are added.

`tools/ci/arm64_jit_fail_closed_gate.sh` completed successfully, including all
four assertion/release ARM64/ARM64e M7 FFI profiles. The final linked runtime
tests report `t-vm-safepoint OK` and `t-jit-recorder-safepoint OK`; these are
actual runtime executions in addition to the source contract checks. The
upvalue identity, source/import loop-cell and native-coverage checks also
pass. The outer runner reports `gate exit=0; restore exit=0`.

The gate checkout is restored to ordinary assertion ARM64 without helper
defines. The separately completed ordinary ARM64 and strict `-Werror` x86_64
matrix passes all 509 stock source cases and all 509 original v2-bytecode
cases on each architecture, plus the workload and topology comparisons
described in `arm64-num-child-byte-ordinary-checkpoint-2026-10-30.md`.

Evidence directory:
`/Users/frityet/Scratch/ljmt-num-child-byte-integration-20261030/`.
`gate-source.txt` identifies the frozen revision, `complete-gate.log` records
the full run, and `gate-restoration.log` records the ordinary restoration.
Notes added after the frozen revision do not change production, tests, or CI
scripts; that equality was checked before promotion.

This checkpoint does not establish full x86_64 parity. Later NUM-loop integer
bodies, constant-negative and dynamic-positive byte ends, and variadic sret
aggregate-prefix cases are separate changes under validation. NUM loop child
traces, wider forwarding, general Lua call/return, recursion, protected calls,
and remaining FFI compositions are still active work.
