# Retained cdata comparison for native C-library lookup

Shared captured-builtin namespace lookup now compares its current cache value
with the exact cdata retained by the executing trace. This removes redundant
result discovery and publication while retaining the source authority and
close checks introduced by `aee88db5`. On top of the GC-control repair in
`84378609`, all 1,174 combined functional processes pass. Seven matching pairs
measure lookup at 251.20 to 167.43 ns, a 33.37% median paired reduction.

The source change is limited to `lj_tab.c`, `lj_tab.h`, `lj_crecord.c` and
`lj_ircall.h`. The frozen patch is
`e8175c5b2c4a86a87caba35336ac1bc5bcdfdeeed88492fb0fe5b0ede9530d62`.

## Authority and lifetime

The shared recorder's nonnumeric C-library path passes an actual typed cdata
`IR_KGC` to `lj_tab_cmpcdata_kgc_rooted_try`. The helper requires the exact
physical state owner and a continuous published native interval. It acquires
SMR once before observing source words, retains the original table/key leases,
and uses the existing paired array/hash resolver and generation checks. Source
words, owner and native base are confirmed before cleanup.

The result inspection uses only an acquired local TValue's tag and pointer
bits. A match names the independently retained trace object. An unmatched
object is never dereferenced, validated through its header, leased or
published. The helper writes neither source nor output scratch and balances
all acquired resources on refusal. The trace's KGC graph and existing
native/phase/retirement protocols provide expected-object lifetime; merely
passing a recently observed raw cdata pointer would violate the contract.

The generated call remains side-effecting `CALLS` with `CCI_L|CCI_T`. Its
immediate snapshot/status guard preserves preceding Lua effects. A fresh
signed volatile close check follows before value export or extern access.
Numeric overrides still load their actual current value through the old
helper, including signed zero, NaN and infinities. Recording-time lookup and
its three-anchor rollback also retain that helper.

The ordinary small table/string/cdata source path removes one result lease,
four nominal shared RMW operations, one explicit SC fence, layout discovery,
and the generated result load/identity check. Ten nominal RMWs and two fences
remain. These are source counts, not hardware profiling. Existing CAS retries
remain; this is not a wait-free claim. Allocation, callbacks, polls or native
quiescence inside this helper would require a new lifetime design.

## Validation

The independent aee-based candidate passed 594 functional processes. Its
source, fixtures and results were frozen before measurement. ROOT combined
the exact four files with `84378609`, checked all 224 tracked runtime/generator
inputs in three fresh trees, and ran:

| Combined checks | Processes | Result |
| --- | ---: | --- |
| Cache authority, numeric/growth, close and environment lifetime | 444 | Pass |
| Native wrong/null, SMR, GC/flush and source-lease refusal | 114 | Pass |
| Trace-only cdata retention with independent JIT-off controls | 36 | Pass |
| Stock and broader receiver, userdata/cdata, callback, recorder and flush regressions | 294 | Pass |
| Shared canonical automatic-GC control | 37 | Pass |
| Shared canonical cache authority and recorder cleanup | 153 | Pass |
| New shared canonical comparison and retention | 96 | Pass |
| Total | 1,174 | Pass |

The first 888 processes run across normal, APICHECK/assert/helper and Clang
target-only ASan/LSan builds. Stock reports 387 JIT-off and 509 JIT-on assertions
in each build; these assertions are not extra process counts. The new
canonical case runs 42 default and 54 helper/assert processes, then restores
the default build. Both source generations total 1,768 passing functional
processes. Builds, IR probes, development pilots and benchmarks are counted
separately.

Native probes require the old root or installed side to execute, preserve
exact prefix counts and extern targets, and inspect real resource cleanup.
The SMR control pauses a real reclaimer after exclusive admission closes and
observes its genuine native-active veto. The GC control pauses a real public
collector's corresponding deferral decision; public flush provides a real
pending request. No runtime gate or ownership plane is fabricated. Table/key
refusal uses existing one-shot admission hooks. The removed result lease is
not claimed as retained refusal coverage.

The lifetime fixture closes the namespace, removes its authoritative value,
drops strong result aliases, and advances real retirement until the namespace
cache head and global retired-cache list are empty. Two further full
collections clear an unrelated weak table. Only JIT-on retains the weak cdata;
all JIT-off controls clear it. The fixture then restores that exact weak value
to the cache and proves real native comparison success followed by the close
error, with no closed extern store or prefix replay.

Existing authority/supplement witnesses select the new helper only for cdata;
numeric witnesses and recorder-root wrappers keep the old helper. The
between-close wrapper calls the real comparison before closing. One permanent
C fixture serves both new Lua entrypoints, using the exact validated
retention-C bytes; the canonical run checks both routes.

Earlier fixture failures remain archived: a Linux compile used an unavailable
platform accessor, an initial GC observer incorrectly expected collection to
block, and an IR observer displaced the raw dump listener. Corrected fixture
generations required no runtime source changes. The separate attachment
experiment rejected by automatic safety review remains unperformed, as
recorded in the preceding cache-authority review.

## Measurement and identities

Both studies use seven alternating pairs on CPU31, GC enabled, a real attached
peer, and five retained passes of two million lookups per process. Each
process requires actual native execution and the expected helper call. No
sample was discarded.

| Source pair | Baseline median ns | Candidate median ns | Median paired change |
| --- | ---: | ---: | ---: |
| `aee88db5` and isolated comparison | 251.42 | 167.71 | −33.32% |
| `84378609` and combined comparison | 251.20 | 167.43 | −33.37% |

The combined ranges are 250.832–251.870 ns and 166.984–167.964 ns; its paired
geometric ratio is 0.666347815. Root IR/native bytes change from 39/515 to
35/438 for function lookup, 41/546 to 37/469 for extern read, and 41/530 to
37/458 for extern write. Numeric remains 37/473. The two static calls are the
preheader and loop body; ordinary iterations perform one call. These
measurements establish neither stock parity nor application/CALLXS speedup.

The shared default CLI is
`226deab70e1ed8608a6f13d90c8445e5e5ee945087868e85261f656588d2d385`,
archive
`d7278ecb35007efc8e0b6e799909047b8f795db26499ee7297f940144c40952d`,
and shared library
`189e885f3c32a044ac1458743f5987675eac1282d5e7c195896e5689eb933944`.
Isolated ELFs differ because the archive fallback version differs from the
workspace Git timestamp. Exact generated metadata differences and matching
compiler/table/GC/API/VM object hashes are preserved; no identical-ELF claim
is made across that difference.

Evidence is under [evidence/ffi-clib-cdata-compare-2026-09-05](evidence/ffi-clib-cdata-compare-2026-09-05/).
Its manifest binds the source-cost review, frozen code/IR review, independent
candidate package and combined ROOT validation. Full benchmark samples are
also under [bench/ffi-clib-cdata-compare-2026-09-05](bench/ffi-clib-cdata-compare-2026-09-05/).
Only verified UTF-8 text is copied; binaries and source archives are hash-only.

General shared metamethod/cdata recording, concurrent table migration,
asynchronous GC and stock performance parity remain open. Worker GC completion
and unfinished-owner progress remain stability priorities. Linux only was
validated; this change does not establish release readiness.
