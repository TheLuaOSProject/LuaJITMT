# x64 JIT atomic bytecode redispatch

## Failure

Concurrent full `jit.flush()` and root re-recording could produce wrong numeric
loop results, invalid interpreter PCs, crashes, or hangs. The reproducer needed
a live peer repeatedly executing a hot root while another thread restored and
re-published that root's bytecode.

The shared x64 return block used by `lj_dispatch_ins`, `vm_hotloop`, suppressed
hooks, and `cont_hook` decoded the current instruction with separate loads of
`PC_OP` and `PC_RD`. Trace publication and retirement replace the complete word:

- `FORL` stores a biased branch offset in D;
- `JFORL` stores a trace number in D.

A full-word `bc_publish()` between those loads could therefore pair `FORL` with
a trace number or `JFORL` with a branch offset. All involved C-side writers were
already release-publishing one aligned 32-bit `BCIns`; the x64 reader was not
preserving that publication unit.

## Change

The shared return block now loads `[PC-4]` once and decodes opcode, A, and D from
that register image before static redispatch. An aligned x64 32-bit load cannot
tear against the existing full-word publications, so a returning VM observes
one complete bytecode generation. This also removes multiple loads from the
common block rather than adding synchronization or a lock.

The audit found no other reachable split opcode/D redispatch. Safepoint,
profiler, trace-exit, and stale-JLOOP paths already decode a complete word.
`vm_hotcall` reloads only A, which is invariant across `FUNCF`/`JFUNCF`, and its
later trace-number validation fails closed.

## Adjacent follow-ups

Two separate patch-site races were exposed by the audit and remain explicit
follow-up work:

- `ITERN` loads the following instruction's D while assuming it is still
  `ITERL`; concurrent `ITERN` despecialization and `ITERL`/`JITERL` publication
  need a complete-word validation rule.
- `ISNEXT` checks the next opcode and later publishes `ITERC` unconditionally;
  a conditional full-word publication is needed so it cannot overwrite a
  concurrently published `JLOOP` while retaining that trace-number D.

Neither is on the diagnosed `vm_hotloop` return path, but both must be resolved
as part of the remaining concurrent JIT patch-site audit.

## Regression coverage

`m6_jit_flush_atomic_redispatch` shares one hot numeric loop between a worker
and a full-flush/re-recording thread. It checks every result while repeatedly
cycling the same `FORL`/`JFORL` word. The pre-fix binary reproduces wrong
results, invalid values, crashes, and timeouts; the patched binary completes
cleanly.

Validation performed on x86_64 Linux:

- pre-fix `912a1eca`: five failures in six calibrated runs (two timeouts, a
  wrong result, a corrupted-value error, and a segmentation fault);
- patched assertion/paranoia build: 200 consecutive focused reducer passes;
- patched calibrated regression: five consecutive passes;
- default-build M6 cases: `m6_jit_flush_atomic_redispatch`,
  `m6_jit_flush_hs`, `m6_jit_flush_thread_stress`,
  `m6_jit_flush_thread_heavy_stress`, and `m6_jit_util_flush_race`.
