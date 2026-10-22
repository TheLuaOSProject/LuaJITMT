# ARM64 VM dirty-epoch experiment

The VM parity audit did not establish a new correctness gap requiring a VM
change. An inline replacement for `arm64_vm_stack_dirty` was measured and
rejected; production still calls `lj_state_stack_dirty_vm`.

The helper resolves the executing TG and performs a relaxed atomic 64-bit
increment of `stack_dirty_epoch`. It does not allocate or enter GC. Publication
of the new TValue and its GC barrier remain separate caller responsibilities.
An inline implementation must retain the atomic RMW and uint64 wrap semantics;
the x64 plain increment alone does not justify an ARM64 load/add/store sequence.

## Measured candidate

The candidate used two address additions, a constant move, and relaxed `LDADD`,
with the existing helper as the non-LSE fallback. LSE was selected from
`TARGET_TESTARCH` into a DynASM define. Testing a host compiler builtin in
buildvm would incorrectly select host features for cross or generic targets.

Nine alternating fresh-process pairs, JIT disabled, 50,000,000 iterations per
case, measured CPU time with `os.clock()`. Both builds used assertions and
`LUAJIT_MT_ARM64_BOOTSTRAP` plus `LUAJIT_DISABLE_JIT`. The source snapshot was
`3a11a2ae`; the only production candidate change was the inline epoch bump.

| Case | Helper median (s) | Inline median (s) |
| --- | ---: | ---: |
| Rotate four local table references | 0.819736 | 0.844840 |
| Identity function call/return | 0.589471 | 0.586705 |
| Assign one string constant | 0.198248 | 0.199944 |

The move case became about 3.1% slower, while the other differences were small.
There was no reliable performance win. The production, DynASM, build-system,
contract, and temporary benchmark changes were all removed.

## Stress observation retained for follow-up

The runtime root-publication and safepoint fixtures passed for the candidate,
as did source/object publication contracts. A temporary exact epoch-count test
also passed normal increments, wrap, and competing atomic increments.

An earlier `STADD` candidate twice failed concurrent tmpbuf stress with:

    LuaJIT ASSERT lj_gc2.c:6505: lj_gc2_sweep_to_idle: table NEEDSCAN leaked to normal sweep close

The same fixture was then run against the preserved unchanged baseline:

    src/luajit -joff tests/t-arm64-tmpbuf-thread.lua

One baseline run passed; a later fresh process exceeded 90 seconds with empty
stdout/stderr and was terminated by the test driver. This is baseline stress
instability, but it does not reproduce the assertion or establish a shared
cause. Three later fresh runs with disposable diagnostic instrumentation in
table rescan reservation and sweep close passed within a 45-second limit and
reported no late reservation. No diagnostic instrumentation was retained.

The fetched `origin/v2.1` adds 64 commits after `a649f737`, including GC2
coalescing and worker scheduling fixes. Its `lj_gc2_sweep_to_idle` body is
byte-identical to the port's body (SHA-1
`bf2e6c82623c31582df65b259b7b759aa28a198b`). Commits `997c0044`, `09d09e63`,
and `d680421c` affect relevant surrounding mechanisms, but no specific upstream
fix was established for this observation. `1bce0fa5` changes arena/table
markword authority, not the TG dirty epoch. A broad import is not justified by
these results alone.

Disposable local evidence was retained at
`/tmp/luajitmt-vm-epoch.8qSRtz`: `before/luajit`, `benchmark-ldadd.json`,
`benchmark.lua`, build logs, the rejected patches, and the three diagnostic
run outputs. Temporary-directory retention is not a repository guarantee.
