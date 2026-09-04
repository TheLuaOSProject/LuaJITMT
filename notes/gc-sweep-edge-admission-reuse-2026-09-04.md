# Retain SWEEP TValue admission through marking

Date: 2026-09-04. Linux x64 stability and performance review.

`gc2_trace_sweep_tv_edge()` used to acquire an observational body lease, then
call the generic semantic marker while retaining that lease. The marker
repeated the allocator lookup, exact-header validation, and reader admission.
The new path passes the existing counted scope, validated type, and small
allocation start to `gc2_markobj_preserve_admitted_status()`. It keeps the
original scope until direct-body preservation and semantic publication or
recovery have finished.

This changes admission reuse, not the public barrier's graph contract. Public
SWEEP table requests must still reach traversal after a raw payload write;
only private graph discovery may suppress a table which was already scanned.
The separate SSB conversion and observational FINREG membership fixes are
present in both versions used for this review's optimization control.

## Lifetime argument

- TValue admission remains observational: stale addresses and wrong tags
  cannot gain a mark or recovery entry. Exact cdata allocation geometry is
  checked before semantic marking, including headers inside variable or
  over-aligned allocations. A transient admission failure still retains its
  exact recovery identity or pins reclamation.
- A small allocation keeps the counted arena admission which established its
  header identity. The existing `gc2_mark_small_cell_admitted()` performs the
  same mark/rescue operation and final lifetime checks. PREPSWEEP and reopened
  committed generations accept durable marks; terminal bitmap application
  remains read-only. Late free and DESTRUCT transitions cannot be converted
  into permission to access a dead body.
- A huge allocation keeps the slot reader and stable HugeTab header from its
  exact observational admission. A local `HugeTab` view of that header is
  sufficient for the metadata mark CAS; it neither reacquires a body reader
  nor depends on the former TG's embedded wrapper. The reader prevents
  retirement, destructive close, realloc, and physical header reclamation.
  The mark CAS still rejects later external-free intent (`DEFER_FREE` or
  `FREEING`).
- NEW-mark rescan-state reset, direct table-vector preservation, existing
  worker graph filtering, and durable publication/recovery remain in their
  previous order. The scope stays held across all of them. An admitted edge
  whose phase changed before marking follows the existing phase path; a
  custom allocator's no-op observational scope retains the old compatibility
  path and gains no new allocator identity assumption.

## Deterministic coverage

`tests/t-gc2-sweep-edge-lease.c` invokes the production
`lj_gc2_barrier_tv_g()` path. Linux linker wrappers count its real allocator
admissions and inspect retained-reader state. It verifies:

- one small admission, exact tag/stale rejection, direct array/hash vector
  preservation, and eventual marking of the parent's only child;
- five committed or bit-only admission states, including terminal no-mark
  behavior;
- a pause after the mark followed by a DESTRUCT collision and restoration to
  LIVE, with the exact recovery item retained and the child later scanned;
- real small external free while the reader remains held, preserving the
  late pin and recovery ownership;
- one huge observational admission, no nested huge mark-reader admission,
  one metadata mark and one release, including the final available reader
  slot, rejected retirement/destruction, and a temporarily overwritten former
  TG wrapper;
- huge external-free intent after admission, which cannot gain a mark or
  bypass the terminal ownership transition;
- fixed, variable, and over-aligned cdata geometry, including a deliberately
  mismatched fixed-size type and both small and huge interior headers.

The fixture uses synthetic phase and arena-state setup for the indicated
transitions, then exercises production barriers, allocator handshakes, and
queue/recovery drain. It does not claim an exhaustive concurrency proof.
Its canonical `m3_gc2_sweep_edge_lease` registration enables runtime assertions
and test helpers, and explicitly runs only on Linux because its linker
wrappers are not a portable cross-platform fixture. Windows and macOS work
remains deferred until release preparation.

## Validation

The isolated source snapshot is based on
`ea23bf0c0490284b250fe5cbbf4e3f0b8a1fcdb0`, with the shared GC admission,
SWEEP SSB, and FINREG fixes overlaid. Source hashes, exact fixture compile
commands, exit statuses, optimization-only reverse patch, and logs are in
`/tmp/lj-gc-sweep-lease-20260904-2p5yleu8/`:

- `validation-snapshot.json` identifies the combined source and control.
- `strict-results-latest.json` records GCC 14.2.0 assert/helper runs. The
  complete edge, traversal, and recovery fixtures pass. Each fixture was
  compiled with `-std=gnu11 -O2 -Wall -Wextra -Werror -mcx16` in addition to the
  matching helper/assert flags.
- The same unmodified edge fixture fails with the original admission path at
  `test_small_tag_and_direct_bodies()`'s `watch.small == 1` assertion: that
  path acquires the small arena twice. The control retains the SSB guard,
  observational FINREG predicate, and all three tri-state GC consumers. This
  negative control establishes which admission was removed; it does not
  classify the former duplicate admission itself as a correctness defect.
- `asan-results-latest.json` records Clang 19.1.7, `-O1 -g`, frame pointers,
  runtime assertions, helpers, and AddressSanitizer. The complete edge,
  traversal, and recovery fixtures pass with
  `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`; no runtime suppression is
installed.
- `canonical-edge-latest.log` records the passing canonical suite entry and
  successful restoration of the isolated default build.

The frozen original-admission control's GC source is byte-identical to the
subsequently committed stability revision
`09d09e632803edbc207f91951779b9706f73b640`. Applying the saved
`lease-optimization.patch` to that revision yields exactly the tested GC
source, SHA-256
`03642a1d466a127af219e9654bfc4e2a4ab63fe9119722a7685528df1d5a84f9`.
`optimization-patch-check.json` records that check. The
[checked-in text artifacts](../bench/gc-sweep-edge-admission-2026-09-04/)
preserve the manifests, commands, fixture results, and diagnostics described
here; generated binaries and raw DWARF stack captures remain local.

The first sanitizer build stopped because the instrumented host `buildvm`
generator intentionally retains process-lifetime allocations until exit.
The completed build used `ASAN_OPTIONS=detect_leaks=0` only while running make
and its generators. Runtime fixture execution explicitly re-enabled leak
detection as recorded above. No runtime result from the failed generator
build is counted as a pass.

## Ordinary-workload regression found during measurement

No seven-pair timing result was obtained. Both normal static GCC builds were
cleanly rebuilt without assertions, test helpers, or sanitizers, and differed
only in the admission-reuse GC source. The very first control process hit its
90-second outer timeout:

```sh
BENCH_SCALE=0.05 taskset -c 30 "$control/src/luajit" -joff \
  "$control/plan/aux/bench/bench.lua" tab_insert_newkey
```

`BENCH_GC_MODE` was unset, and Lua module paths pointed at the control tree.
The unmodified harness requests 10,000 insertions per internal iteration,
with five iterations and a full collection before each. Its setup also
constructs the other workloads' shared key tables before applying the filter.
No completed row was saved: the failed timeout wrapper did not persist its
buffered partial stdout. The reached harness stage was therefore unverified.
The attempt was converted to a diagnostic after it exceeded the intended
measurement window. Other agents then resumed their work, so this is not an
isolated timing sample.

A two-second, 49-Hz profile captured 100 samples without loss. A separate
unstripped relink had the exact measured executable's build ID
`7939ae9c88767433f5b2dd6158138616d8676a8f`; it supplied symbols without replacing
the running executable. Approximately 96% of the profile was under
`lj_BC_CAT` / `lj_meta_cat` -> automatic GC step -> worker table traversal,
and approximately 93% passed through SWEEP TValue edges. This identifies
active traversal work; a profile alone does not prove either convergence or
the number of rescans.

The follow-up `insertion-progress.lua` creates only one table, inserts 10,000
string keys, then explicitly collects. A small C module reports the Lua index,
process CPU time, phase, cycle, queue counts and recovery counts without Lua
allocation. Both runs completed:

| Observation | Original admission | Reused admission |
| --- | ---: | ---: |
| Index 10,000, process CPU seconds | 33.690064 | 20.468703 |
| Recovery entries at that point | 8,957 | 9,989 |
| After explicit full collection, CPU seconds | 33.882583 | 20.481286 |
| Final phase / recovery entries | IDLE / 0 | IDLE / 0 |

These are instrumented single diagnostic runs with differing GC schedules,
not a statistically established speedup. The index advances during one SWEEP
cycle while the backlog grows. Full collection after mutation stops converges;
the observed case is severe work amplification, not a permanent stall of one
insertion. Admission reuse reduces repeated work without fixing the source of
that amplification.

A second, bounded 5,000-key diagnostic adds recovery lane/accounting totals
and samples at most 256 hash nodes once. It links a small host against each
normal archive so the internal admission APIs are available. The sampler
holds SMR plus an observational table lease for the vector reads and an exact
STR lease for each inspected key; it does not walk remote owner lists or
interpret unadmitted payload bytes. At index 4,250, the original-admission run
reported 3,194 recovery entries, zero huge entries, idle main-thread recovery,
and one pending table. Five sampled keys were live, ready, already-marked small
strings with PENDING recovery, no late-free intent, and no intrusive root.
At index 5,000, both versions reported 3,925 outstanding entries:
15,965 published minus 12,040 drained, with zero REDIRTY or failed recovery.
The final collection balanced publication/drain totals and cleared recovery
and pending-table counts.

This evidence exposed unnecessary semantic publication of leaf strings in the
SWEEP path, in addition to table-request coalescing costs. Their correction is
separate work with its own semantic and convergence checks. This admission
change alone does not resolve the ordinary-workload regression, and no
performance acceptance gate is claimed here.

These checks cover the changed admission path and its GC interactions. They
do not prove the repository's larger fully nonblocking GC, JIT, or FFI goal;
the remaining owner-dependent protocols and performance gaps stay open in
the completion plan.
