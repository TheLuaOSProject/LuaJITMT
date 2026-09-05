# Persistent wide table-scan authority, 2026-09-05

Ordinary 32-bit table dirty-counter rollover now promotes to wider persistent
authority instead of permanently vetoing reclamation for the whole universe.
The ordinary inline stamp remains 16 bytes, including the existing 8-byte
exact rescan token. The common barrier still uses CAS64. A separate 16-byte
proof stores a 64-bit era, 32-bit serial and covered GC cycle after promotion.
Exhaustion of the full era/serial pair retains the safe absorbing veto;
the separate GC-cycle exhaustion policy also remains unchanged.

Wide storage is reserved before publication. Small traversable mappings have
a dense second sidecar plane, making their sidecar 128 KiB rather than 64 KiB.
Huge traversable mappings use a 16-byte physical tail reservation with no
separate heap allocation. Plain Huge mappings share the checked physical
geometry but leave the proof pointer NULL. Header size, first cell, inline
stride and VM/JIT emitted resets remain unchanged. Failed reservation remains
private; a completed write never needs to allocate wider proof storage.

Promotion increments the persistent wide identity and clears its coverage
before publishing the inline sentinel. A paused publisher owns no initializer
state: another publisher can invalidate and promote independently. Scanners
capture their inline/wide domain before reading payload and must match that
domain, serial and era when publishing coverage. An old inline scan cannot
adopt a newly published wide proof, and a serial repeated in another era
cannot validate an old scan. Private small-cell reuse resets only inline
coverage; wide identity persists for the mapping lifetime. Pending rescan
tokens continue to prevent reuse.

Wide reads require retained readable-body authority. Small FREE and Huge
DEFER_FREE token completion remain header-only and do not inspect the proof.
Huge extent includes header, exact logical payload and tail, rounded to 64 KiB
with checked arithmetic. Logical lookup/bounds, copying and live accounting
exclude the tail. Published traversable realloc retains its refusal gate;
the private resize path and published plain reader/deferred handoff retain
their previous ownership contracts.

The four production files are byte-identical to the final frozen
[Huge-tail study](gc-huge-tail-overflow-prototype-2026-09-05.md), whose source
patch SHA256 is `dca24e13fdc47d886a1ffc07b42a604c4e430090fa5f2fd9b1f8111678a8ee4a`.
The permanent tests extend the current fixtures, including the truthful FNEW
repair, rather than importing obsolete prototype fixtures. The two test-only
namespace helpers explicitly require retained/private storage and exclusion
of concurrent proof updates.

Five strict fixtures and four canonical cases pass on `ff2a6ca0` plus this
patch: coalescing, full traversal, full TNEW, full repaired FNEW and Huge-tail
storage. Coverage includes both mapping kinds; inline/wide legacy/exact old
scanners; paused publishers before and after mode publication; twelve forced
rollover/full-GC rounds per kind with weak garbage disappearing; promotion
while calloc is denied; both high allocation cells 1536/1537 with neighboring
proof/token guards; terminal namespace containment; and real PROT_NONE proof
pages during header-only completion. The new `m2_arena_huge_tail` also checks
all boundary payload bytes, untouched-tail residency, logical bounds, map and
locator failures, resize, retained readers, transfer and whole-map unmaps.
One helper precondition comment was clarified after those runs; executable
fixture text is verified unchanged.

The final combination is `4e7a270e` plus this patch and the native cdata-method
guard correction subsequently committed as `30cf1d99`. All 792 tracked
source/test/build inputs match across fresh normal, assertion/helper and
target-only ASan trees. Each passes stock 387 interpreter and 509 JIT tests,
cdata Lua and eight native method-guard modes in both modes, the default-count
concurrent table/metatable/weak/finalizer matrix, JIT table cases and both
native activation controls. Strict and ASan each additionally pass 25 C
executions, including full traversal/FNEW, all 13 cdata capture schedules,
recovery, store guards, FINREG roots and paused root-ABORT retirement.

The final run contains 142 successful bounded commands and 113 test processes,
with no timeout or sanitizer report. Target-only Clang instrumentation is
verified in the runtime objects and absent from host generators. Every ASan
runtime uses `detect_leaks=1:abort_on_error=1`, without suppressions. The known
shared-cdata JIT hammer line-80 refusal remains excluded and failing by prior
evidence; no assertion or native coverage requirement was weakened. These
durations are functional bounds, not cost measurements.

All 210 shared production files match the validated final source. The shared
default mixed build also completes, and both new cdata capture/native
method-guard Lua fixtures pass with JIT off and on. Independent source review
found no concrete blocker in promotion, reuse, token-only completion or Huge
geometry. The full commands, source/fixture/binary identities and qualification
are in [combined results](evidence/gc-table-wide-authority-2026-09-05/combined/RESULTS.md).

The storage choice has a documented cost. The isolated dense study measured
ordinary barrier paired median +0.26%, promoted barrier +61.57%, and up to
roughly 1.3% in its ordinary workloads. Dense metadata doubles reserved
sidecar bytes; promoting many cells can fault additional pages. Tail storage
removes the Huge proof's allocator call/chunk, but 16 size residues per 64 KiB
quantum require another virtual mapping quantum. A proof on a separate page
can add 4 KiB resident memory per promoted Huge object. Some measured tail
workloads regress; full samples and bounds are in the
[dense study](gc-table-dense-overflow-prototype-2026-09-05.md) and
[tail study](gc-huge-tail-overflow-prototype-2026-09-05.md). These isolated
measurements are not final combined throughput or stock-parity evidence.

This removes the practical per-table 32-bit authority exhaustion dependency.
It does not remove general SMR, native acknowledgement, global worker,
table-migration, OS mapping/page-fault or other outstanding progress limits.
The evidence is compressed adversarial counter schedules plus ordinary GC,
not billions of naturally executed mutations or an exhaustive memory-model
proof. [Integration evidence](evidence/gc-table-wide-authority-2026-09-05/)
preserves exact production/test identities, commands, assertions, limits,
independent review and the shared build.
