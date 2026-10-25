# ARM64 scalar FUNCF admission

The experimental ARM64 root-entry profile now treats a fixed-arity `FUNCF` body as a bounded scalar program rather than requiring the old `KPRI true; RET1` template. Its bytecode walk accepts initialized parameter/local reads (`CGET`, `MOV`), primitive and short-integer constants, scalar `ADDVV`/`SUBVV`/`MULVV`/`DIVVV`, and a terminal one-result `RET1`. Calls, upvalues, branches, heap operations, varargs, and unsupported opcodes remain refused. Integer addition is admitted only as guarded `IR_ADDOV`; ordinary integer arithmetic does not receive a certificate.

Semantic and post-register-allocation checks each resolve the returned bytecode register from its latest definition and require the snapshot result to name that same IR value. They also validate snapshot PC order, legal frame slots, scalar SLOAD provenance, and allocator register/spill constraints independently. The publication certificate separately retains the exact FUNCF bytecode generation, so a legal mutation between the last pre-entry check and root transfer cannot substitute different code that happens to fit the same grammar. The final runtime boundary compares the compact body copy with acquire-loaded live bytecode; it does not rerun full IR/register-allocation admission there.

The post-RA proof also computes each supported value's last IR and snapshot use. A destination cannot reuse an older value's register while that value remains live, and a value in the current guard snapshot cannot be clobbered even when its last ordinary IR use is the guarded instruction. Its work arrays are bounded by the admitted bytecode extent. The fixture mutates an intermediate `y=x*a` result to use `x`'s register while a later `x+y` still needs `x`, then restores the original assignment and rechecks admission.

The focused native fixture covers false, nil, integer constant and argument returns; four floating arithmetic operations; multi-local arithmetic; integer overflow side exit; independent post-RA slot, snapshot-location, simultaneously-live input, and later-use register corruption/recovery checks; and a deterministic `POSTMETADATA` bytecode mutation race. On 2026-10-25, the contract passed on native arm64 and arm64e with `LUAJIT_MT_ARM64_BOOTSTRAP`, `LUAJIT_MT_ARM64_JIT_EXPERIMENTAL`, `LUA_USE_ASSERT`, `LJ_TRACE_TEST_HELPERS`, and `LUAJIT_MCODE_TEST`, and again without `LUA_USE_ASSERT`. These are bounded positive proofs for this grammar, not support for arbitrary function bodies.

Combined-gate follow-up: the standalone emitter contract compiles admission
with `-Wextra -Werror`, exposing an unused bytecode-walker argument that the
ordinary build did not reject. Removed that argument and an obsolete forward
declaration; the emitted-instruction contract passes unchanged. Log:
`/tmp/ljmt-review-funcf-emitter.log`.

Both 509-case stock source/original-bytecode corpora passed on native ARM64
at `05c8269b`, before the additional allocation-interference negative. Logs:
`/tmp/ljmt-review-funcf-stock-source.log` and
`/tmp/ljmt-review-funcf-stock-import.log`. The last-use contract additionally
passed on ARM64/ARM64e with and without assertions in isolated matching builds:
`/tmp/ljmt-funcf-live-default.log`, `/tmp/ljmt-funcf-live-release.log`.
