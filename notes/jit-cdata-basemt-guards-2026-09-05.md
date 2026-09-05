# Native cdata base-method guards, 2026-09-05

Changing an existing cdata base metatable's method could leave native traces
executing the previous method. In the exact baseline control, a replacement
`__call` ran only once instead of 80 times, even though the arithmetic result
still matched. Interpreter execution called it 80 times. The same stale
dispatch affected `__index`, `__newindex`, and missing/nonfunction methods.

`lj_record_mm_lookup` now routes cdata through the existing base-metatable load
and guarded table lookup. The removed special case treated the method as
immutable. The general recorder's structural, key/value and function guards
now invalidate that assumption at execution. Special userdata behavior and
the existing MT recorder refusal remain unchanged. This is a correctness fix
for the admitted pre-MT path; it does not enable shared-MT cdata recording.

The exact isolated baseline is `dd2c439179b1e12564710484d8511e4cee617f7f`;
only `src/lj_record.c` differs in production. Final source SHA256 is
`e5d872f8fc9af3fe10643f6a328084d0d582c3a9753bb70bcccced1a0e65ef6e`.
The source/object manifest covers all 224 tracked source/DynASM files across
four trees. The interpreter capture optimization landed separately and is
absent from these isolated measurements.

The permanent `m6_jit_cdata_basemt_guards` case runs eight modes with JIT off
and on: three changed methods, missing/nonfunction `__call`, hash growth with
collection, recorded Lua-method lifetime, and base-root replacement. Native
controls require actual TEXIT evidence and exact replacement counts. The
missing-method loop enters native code before the first cdata operation, so
an interpreter error cannot substitute for guard coverage. The lifetime case
retains only a weak Lua reference after replacement and proves that the trace
keeps its recorded method alive across full GC. Normal, assertion and final
canonical runs pass. The exact baseline fails the in-place mutation controls;
base-root replacement already flushes traces and passes both versions.

Fresh baseline and corrected normal builds each pass stock 387 interpreter
and 509 JIT tests. Targeted FFI read/write/finalizer cases, the original native
mutation control, xbar/XPOLL, first-thread activation and first-GC-worker
activation also pass. Recording/root publication continues to rely on the
existing pre-MT exclusion and trace-root lifetime protocol. Extending this
lookup to MT needs exact source capture, rooted nonwaiting native guards and
base-root publication exclusion; removing the existing refusal is unsafe.

The unchanged shared-cdata hammer still fails line 80: constructor recording
reaches the MT metamethod refusal before its trace-owned allocation exception.
This is a real missing-native path, not a trace-count observation error. The
failure is preserved. A speculative flush-callback publication race was not
reproduced by the separate probe and is not claimed as a finding.

The fix has a measurable cost. Seven alternating fresh pairs per workload
use matching default normal builds on CPU 31, GC enabled, on a shared host.
All 42 processes complete; no samples are discarded.

| Workload | Baseline median best | Corrected median best | Median paired change |
| --- | ---: | ---: | ---: |
| Unchanged `ffi_struct`, 30 million iterations | 0.0206 s | 0.0684 s | +232.04% |
| Constructor, sinking allowed, 3 million iterations | 0.004145 s | 0.006879 s | +65.96% |
| Constructor, sinking disabled, 300,000 iterations | 0.044394 s | 0.044937 s | +1.20% |

The first row is roughly 0.69 to 2.28 ns/iteration, with the harness's four
decimal-place reporting resolution. Separate IR/mcode diagnostics show
base-root/node loads and method guards repeated after XPOLL; the tiny field
trace grows from 198 to 501 bytes. These correctness checks stay in place.
Any later guard coalescing needs its own alias, callback, collection,
activation and side-exit proof. These results do not meet the final performance
acceptance gate or describe physical allocation when CNEW is sunk.

[Functional evidence](evidence/jit-cdata-basemt-guards-2026-09-05/) preserves
the independent review, commands, hashes, original failures, rejected stripped
GDB setup and earlier fixture versions. The early fixture's absent-key cleanup
could preallocate capacity; its results are not hash-growth evidence. Corrected
growth and final lifetime controls are separately identified.
[Benchmark evidence](../bench/jit-cdata-basemt-guards-2026-09-05/) contains
every cost sample and matching build/source records.
