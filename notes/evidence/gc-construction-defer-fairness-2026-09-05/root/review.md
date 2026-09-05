# Root integration review: constructor deferral with fair TG continuation

The four-file `a62a8251ba8315f8ce07871635f1367659be3136895102776955c7204a4e018a`
patch is accepted on exact runtime 79345529. Shared HEAD at integration is
a983e643, whose additional changes are fixture maintenance and documentation.
The isolated archive contains 224 runtime/generator inputs plus the root
Makefile. Only lj_gc.c, lj_gc.h, lj_gc2.c and lj_obj.h differ after applying the
patch. Application needs only line offsets; no manual semantic merge occurred.
Current automatic-GC control and retained-cdata lookup changes remain present.
The independent worker SWEEP scheduling prototype is absent.

## Ownership and progress

The unchanged scanner's LINKING/UNLINKING skip now reports an optional local
intrusive-owner refusal. It still retains the allocation and cursor, avoids
unfinished header reads, and preserves pending/EOF/bitmap finish predicates.
The old four-argument entry remains a wrapper. Its caller finishes physical
writer and unseal cleanup before returning the local refusal, keeps real work
accounting, and stops retrying that owner within the current quantum.

Production aggregates those local refusals over a bounded captured TG pass.
A scalar next-TG ID records continuation at quota and finished-arena boundaries,
including when an owner is ineligible. Each invocation resolves that ID through
the currently captured list, falls back to its head on a miss, visits at most
one circular pass, and retains no TG pointer after releasing the worker claim.
The existing inverse worker/SMR/reclaimer checks exclude physical list unlink
through the pass; attach prepends outside its captured head. Existing self-next
repair is treated as the tail. A detached or exhausted ID grants no lifetime
authority. An aggregate defer event then stops higher-level immediate retry,
including the automatic driver's outer batch, after all visited writers exit.

For a stable finite list of N TGs, an admitted target receives a turn within N
invocations that reach the owner traversal. A target requiring K successful
owner quanta therefore receives them within N*K such invocations under those
conditions. Quotas retain their existing meaning: an owner quantum may scan
up to 64 cells or certified no-op words and perform other drain work. Neither
the quota nor this patch is a raw-cell, elapsed-time, handshake or full-cycle
bound. Earlier graph/string/grace work and closed gates can prevent reaching
the TG pass. An earlier held arena can still delay later arenas in the SAME
TG. That remaining scheduling gap is separate work, as is arbitrary continual
attach/detach. The resumed-ID lookup can add a list walk.

The original timeout was observed at a test-injected nested full collect inside
closure construction. No ordinary production Lua trigger is established by
that injection alone. New tests use actual allocation, construction,
publication and cancellation transitions to require nested return with the
same LINKING/CONSTRUCT/READY0 allocation, then actual completion after publish
or cancel. Independent eligible arenas must complete before held owners resume.
No bitmap, list, continuation hint or phase is forged by those new fixtures.

The first rejected deferral patch still fails the corrected independent-owner
oracle in both manual and two-worker controls. The updated oracle permits
legitimate earlier completion during nested collection but keeps the final
pre-release completion assertion. Three-blocker quota-one tests and actual
physical unlink of a naturally hinted TG cover bounded continuation and stale
identity. The old rejected evidence remains in the separate durable archive.

## Validation

The owner preserves 11 focused positive runtime processes and 99/107 broader
candidate runtimes in separate generations on its older pristine597b input.
All eight original broader failures and eight matching pristine controls remain.
The two stale helper assertions pass with the independently accepted v4
fixtures. The original idle-iterator timeout remains separate progress work.
The scheduler's 131,280-byte/6-allocation LeakSanitizer report is independently
traced to the fixture leaving synthetic shutdown set before real close; its
separate accepted cleanup handoff includes current combined assertion/ASan
passes. This integration does not silently reclassify those original failures.

Root current-source validation has 170 positive functional processes:

- 39 initial: stock JIT off/on and generational modes in default,
  assertion/APICHECK and target-only ASan; allocation accounting in each;
  hard-assist and all 11 constructor/retention/fairness cases in each helper
  configuration.
- 82 broader: 41 each in assertion/APICHECK and ASan, covering pending roots,
  arena phases/reclaim, MARK close, JIT cooperation, public rescan, exact edge
  leases, FINREG roots, sole string reclaim, TG lifetime, native root holds and
  completion, local duplicate scans, activation/pacing and Lua peer/finalizer
  workloads. `broad-selection.json` explicitly identifies already-run fixtures
  and the separately investigated scheduler/idle-entry cases.
- 49 registered shared-workspace processes: existing
  `m5_function_construction_anchors` (1), `m3_gc2_auto_control` (37), and new
  `m3_gc2_constructor_defer` (11). All suites exit zero; the last restores the
  default build. The new case is registered in the M3 aggregate too.

Every stock invocation reports 387 tests with JIT off and 509 with JIT on;
those assertions are not inflated into the process count. The new permanent
fixtures preserve the accepted bodies: the base is byte-identical and the
two composite fixtures only change their include to its permanent filename.
Canonical execution checks those exact copies. Existing original-constructor
registration retains its 20-second bound. The root helper builds use ten
recorded defines; the owner's six-helper and seven-helper configurations are
not treated as interchangeable. ASan uses leak detection and instruments
target objects; host generators are checked uninstrumented. Generated JIT
machine code is not thereby sanitizer-instrumented.

Actual Linux x64 layout comparison on default headers reports GC2State=4432,
global_State=5584 and GG_State=42256 before and after. All measured existing
offsets match; the hint fills offset 1164 before grey_top at 1168. The owner's
helper configuration separately reports 4464/5616/42288 with unchanged offsets.
These differing configurations have their own identity records; no portable
layout claim follows.

## Cost and limits

Seven alternating CPU-30 pairs for each of three GC-enabled allocation
workloads give 42 complete benchmark processes and all 210 samples. Summaries
use the median of each process's five samples, then paired ratios. Median
paired changes are +0.18% for interpreted TNEW, -0.28% for interpreted TDUP,
and -0.68% for JIT-enabled TNEW. Ranges overlap. This supports no measured
material change in these workloads, not a speedup or stock-parity claim.
The mixed-owner list-walk overhead has not been separately benchmarked.

Commands, source/header/archive/ELF hashes, stdout/stderr, bounds, source
identity, promotion, layout and all raw cost samples are preserved. Binary
artifacts are hash-only. The runtime still uses synchronous ownership and
root handshakes, global native exclusions and remaining exact-owner waits.
This bounded improvement does not establish complete locklessness, general
leak freedom, Windows/macOS validation or release readiness.
