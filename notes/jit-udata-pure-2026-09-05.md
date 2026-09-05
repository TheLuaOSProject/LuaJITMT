# Direct userdata pure-loop load reuse, 2026-09-05

The special-userdata correctness repair restored mutable metatable and method
guards. Direct receiver loops can now reuse their entry metatable load and its
immediate node-vector load under the existing pure pre-MT loop contract.
`lj_opt_mem.c` accepts only an exactly typed userdata SLOAD or retained KGC;
the node load must come directly from that userdata metatable FLOAD.

The complete original loop body must still satisfy the protected unroller's
existing allowlist. Entry guards, userdata subtype/namespace checks, phase and
TG polls, XBAR and ordinary alias checks remain. Further node loads from a
table-valued `__index` remain inside the loop. Calls, allocations, Lua writes,
indirect writes, profiling and unlisted effects still prevent this reuse.
The only allowed store remains the previously reviewed scalar write into a
direct cdata payload. No broader upvalue, dynamic-key, table or MT exception
is added. The private unrolling flag's error cleanup is unchanged.

The two permanent fixtures cover 36 direct receiver/mutation combinations
with JIT off/on, plus seven late effects that must keep the repeated guards
and one previously allowed scalar cdata store. Native cases require actual
execution, original trace reentry, exact Lua results/call counts and retained
entry guards. Mutation coverage includes method removal/replacement, absent
and nonfunction methods, metatable replacement, resize plus collection,
old-method lifetime and table-valued method entry changes.

The first isolated study passes 742 qualified functional processes across its
guarded control and normal/assertion/ASan candidates. Two deliberate negative
controls fail the hoist or native-execution oracle. An initial compile error,
an incorrectly supplied first-attach argument, and C lifecycle runs compiled
without their runtime's helper macros are preserved; the mismatched C runs
are disqualified even where they passed. All matching-flag replacements pass.

The combined receiver-guard and optimization source passes 261 functional
processes across normal/assertion/Clang-ASan builds, including both stock
modes (387 off / 509 on in each), all captured receiver cases, direct userdata
mutations/effects, previous pure-cdata cases, phase/GC-worker exits, protected
OOM, first attachment with/without LOOP and profiling. All 224 runtime inputs
match between the combined variants and the shared tree. ASan/LSan uses
`detect_leaks=1:abort_on_error=1`, with instrumented targets, uninstrumented
host generators and no suppressions. The new shared canonical
`m6_jit_udata_pure` entry passes all 80 processes in 44.889 seconds including
default build preparation.

[Original proof and controls](evidence/jit-udata-pure-2026-09-05/isolated/review.md),
[combined interaction review](evidence/jit-udata-pure-2026-09-05/combined/review.md)
and [final validation](evidence/jit-udata-pure-2026-09-05/root/final-validation.json)
retain exact inputs, flags, binaries, outputs and failures. Final
`lj_opt_mem.c` SHA-256 is
`00787841d7305b758edacb399e16fe5494b9d923525baa5f6b9523af7acbfc2c`.

Seven alternating fresh-process pairs per workload use CPU 31, enabled GC,
20 million direct lookups and the best of five CPU-clock rounds on a shared
host. These figures measure the original guarded method source and optimizer
candidate, before the separate captured-receiver guard was integrated.

| Direct receiver, constant member | Guarded median ns | Optimized median ns | Median paired change |
| --- | ---: | ---: | ---: |
| C-library namespace | 1.140 | 0.684 | -39.99% |
| File | 2.279 | 1.048 | -53.99% |
| Buffer | 2.279 | 1.048 | -54.00% |
| Plain userdata | 1.545 | 0.912 | -40.99% |

The captured-receiver workloads remain excluded and retain the same normalized
IR. Captured namespace lookup stays about 2.051 ns; actual CALLXS and the
existing `ffi_struct` control remain within measurement noise. Captured file
lookup still varies from about 2.51 to 9.57 ns. Sixteen geometry probes all
find the same depth-one lookup, disproving the proposed collision-chain
explanation; address/layout or host causes remain unresolved. No slow samples
were discarded. [All 112 cost processes](../bench/jit-udata-pure-2026-09-05/cost-results.json)
and [their summary](../bench/jit-udata-pure-2026-09-05/cost-summary.json) are retained.

The combined source adds one namespace entry guard and 20 machine-code bytes;
four additional native shape probes find unchanged hot-loop instructions,
registers and offsets. Timing was not repeated, and these probes are not new
performance measurements. These results establish a narrow lookup improvement,
not full-suite parity, general shared-MT method support or release readiness.
Linux x64 remains the validation target.
