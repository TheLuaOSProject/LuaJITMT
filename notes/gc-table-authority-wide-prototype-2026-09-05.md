# Table scan authority: isolated wide-stamp prototype, 2026-09-05

The original prototype is incomplete and its performance comparison is
**invalid as evidence for choosing a wide-stamp design**. A subsequent source
and generated-object audit found four VM/JIT sidecar index operations still
using the old 16-byte stride after the C stamp grew to 32 bytes. These paths can
read or overwrite another entry's proof or exact token. Passing test logs and
timing samples below are retained as observations of that defective build;
they do not establish a correct integrated wide-stamp implementation. The
compiled layout measurements remain accurate. Production still has the
32-bit table-dirty exhaustion release blocker recorded in plan 15.

The omitted sites in the frozen source are:

| Site | Omission |
| --- | --- |
| `vm_x64.dasc:4541` | TNEW precheck shifts the allocation cell by 4. |
| `vm_x64.dasc:4654` | TNEW post-claim check/reset address also shifts by 4. |
| `lj_asm_x86.h:1582` | Emitted FNEW post-claim stamp base still shifts by 4. |
| `lj_asm_x86.h:1643` | Emitted FNEW precheck stamp base still shifts by 4. |

Field offsets use `offsetof` and did become 16 for the token. FNEW's second
entry delta uses `sizeof` and became 32 bytes per cell, but its starting address
is wrong. For an odd cell index `i`, the mistaken `side + 16*i` address is the
real token of entry `(i-1)/2`; the supposed low-proof reset can zero that token.
For an even index, it resets another entry's proof. The frozen normal VM object
confirms SHL 4 at offsets `0xf57` and `0x10ee` in `.text`, token accesses at
`+0x10`, and a one-qword reset at `0x11e3`. No corrected prototype or replacement
measurement is included here.

The intended C protocol and original experiment are documented below, with
that audit qualification applying to every runtime validation and timing
claim. The detailed inventory and object evidence are in
`emitted-layout-audit.md` and `emitted-layout-audit.json` in the evidence
directory. No claim that the entire collector is nonblocking is made.

All runtime source came from commit
`d680421c4cb50b85437d88255bc89358c5e3a6b1`. The prototype changed only
`src/lj_arena.c`, `src/lj_arena.h`, and `src/lj_gc2.c` in isolated trees under
`/tmp/lj-wide-stamp-20260905-_gc0aoyc`. It excludes the pending scalar-read and
arena reuse changes. No shared production or coalescing source was edited.
The earlier read-only design audit is preserved alongside the compact evidence.

The implemented C proof is one aligned atomic word containing
`{era64, covered_cycle32, dirty32}`. The separate exact rescan token is unchanged.
Under an exact retained table body scope, a normal mutation CAS increments the
dirty serial and clears coverage. At dirty MAX, the same CAS changes
`{E, MAX, C}` to `{E+1, 1, 0}`. This operation both renews the authority and
invalidates the scan after the payload write and before request publication.
A scanner captures both era and serial, and can publish coverage only if both
still match. It cannot adopt a newer era without rescanning. An old scanner
paused at serial 1 therefore cannot publish after a rollover returns the low
serial to 1.

This prototype uses the repository's explicit x64 `la_cas128` for snapshots as
well as updates; the normal GC object contains `lock cmpxchg16b` instructions.
It makes no mixed-width snapshot optimization. Ordinary GC cycles do not reset
the era or serial. Existing phase ownership, proof-before-token-completion,
REDIRTY/COUNTED ownership, worker handling and exact token generations remain in
place. Small reuse clears the new proof only at the existing private-incarnation
point after prior exact body owners are gone and the token is NONE. Huge bodies
use the same proof in their retained mapping header.

Only exhaustion of the full non-wrapping era-plus-serial namespace retains the
sticky reclaim veto and explicit unsuppressible saturated-request handling.
Existing saturation controls were retargeted to that full terminal pair;
they were not removed. The independent global GC-cycle and exact-token finite
namespaces are unchanged. Extending the table namespace does not resolve those
separate boundaries.

The lower-memory split proof/token layout was not a mechanical array change:
the current stamp-pointer API also serves cold token and header-only lifetime
consumers. This bounded prototype retained the simpler layout so it did not
change those access contracts at the same time. The resulting memory penalty
must be included in any comparison:

| Measured layout | Exact baseline | AoS prototype |
| --- | ---: | ---: |
| One stamp, including token | 16 bytes | 32 bytes |
| Sidecar for a traversable small arena | 65,536 bytes | 131,072 bytes |
| `GCAhdr` | 128 bytes | 160 bytes |
| First usable cell | 616 | 618 |
| Token offset in a stamp | 8 bytes | 16 bytes |

The arena plus its sidecar grows from 128 KiB to 192 KiB, or 50%, before other
metadata. The naive header layout also consumes two more cells per arena. These
are compiled `sizeof`/offset measurements, not a measured RSS or peak committed
memory result. A split array of 16-byte proofs and 8-byte tokens would use a
96 KiB sidecar, making arena plus sidecar 160 KiB, or 25% above baseline. That
layout and a tighter huge header were not implemented or timed here.

The deterministic fixture compresses the intervening dirty namespace while an
actual worker is paused immediately before publishing its child-free scan
proof. A subsequent raw child store and the production public table barrier
perform the real rollover. After releasing the old worker, the fixture requires
the new child and grandchild to be marked, a second scan, drained work and no
reclaim veto. Both small and huge tables pass. A separate test retains the same
rooted table through twelve forced renewals, each followed by full collection;
each round reaches IDLE with recovery zero, clears an unreachable weak value,
and preserves the newly stored descendant graph. Both storage kinds pass.

The causal controls are preserved:

- The exact baseline fails the continued-collection test at the assertion that
  reclamation is not vetoed after the real 32-bit exhaustion bump.
- A deliberately broken prototype which compares only dirty32, while retaining
  the new era in its CAS, fails because the actual new child is unmarked after
  the paused old scanner publishes a false proof. This exercises the ABA
  consequence rather than merely observing a changed counter.
- Existing coalescing tests, the full traversal fixture, recovery fixture and
  table-store guard fixture pass. Only their direct authority-saturation
  injections are adapted to force era MAX as well as dirty MAX.
- Both normal baseline and prototype pass stock tests: 387 with `-joff`, 509
  with `-jon`. Clang 19 ASan with leak checking passes all new rollover tests
  and the existing coalescing fixture. The strict builds use GCC 14 with GC2
  and table test helpers and `LUA_USE_ASSERT`.

The initial relocated traversal adapter failed to compile because its fixture
include directory was missing. That failed command is retained; adding the
exact tree's `tests` include directory produced the passing full run. Two
requested CPU-32 timing setup attempts were rejected by `taskset` because this
environment exposes CPUs 0 through 31. No workload ran in those attempts. The
parent approved CPU 31, which was used for all reported pairs.

The performance comparison uses normal builds and the unmodified d680 filtered
`plan/aux/bench/bench.lua`, with `BENCH_SCALE=0.02`, automatic GC, seven fresh
process pairs per case and execution mode, and alternating AB/BA order. Each
harness sample is its minimum of five in-process rounds. The percentage is the
median of the seven paired ratios, not the ratio of the displayed medians.
There were 84 completed process samples. CPU 31 was reserved for this study;
functional work could run on CPUs 0–15 and another study could use CPU 30. The
host and its frequency were not fully isolated.

| Mode | Unchanged workload | Baseline median ns/op | Wide median ns/op | Median paired cost |
| --- | --- | ---: | ---: | ---: |
| Interpreter | Existing table write, 400k operations | 670.16 | 669.26 | −0.13% |
| Interpreter | New-key insertion, 4k operations | 986.25 | 1049.00 | +6.30% |
| Interpreter | Closure/upvalue, 100k operations | 1566.95 | 1581.42 | +1.09% |
| JIT | Existing table write, 400k operations | 2.06 | 2.06 | 0.00% |
| JIT | New-key insertion, 4k operations | 102.50 | 105.25 | +2.46% |
| JIT | Closure/upvalue, 100k operations | 1572.85 | 1591.28 | +1.12% |

Interpreter insertion's seven paired costs range from +6.11% to +7.19%; the JIT
insertion range is +1.21% to +4.65%. The JIT write sample is near the harness's
reporting resolution and is not a direct CX16 measurement. These short filtered
workloads do not establish full-harness throughput or separate atomic cost
from metadata layout and GC schedule changes. No performance preference between
designs can be inferred from this defective binary. A corrected prototype,
explicit emitted-path stride/token/reset controls and fresh comparisons would
be required before using its timing for design selection.

The following alternative is a historical sketch. The subsequent
[reservation audit](gc-table-overflow-reservation-audit-2026-09-05.md)
supersedes its reuse and allocation-failure approach: reserve storage before
publication, retain wide identity across cell reuse, and invalidate it before
the sentinel becomes visible. The post-sentinel invalidation below depends on
resets which existing VM/FNEW allocation paths do not supply.

An optional persistent overflow sidecar is defensible if its initialization and
allocation-failure rules are made explicit. It can retain the existing inline
64-bit proof for the common path and reserve dirty MAX as a permanent sentinel
for that allocation incarnation. Every inline scanner must reject a sentinel
proof; old below-MAX snapshots fail against it. Promoted tables then use a
persistent wide entry, located by exact small-allocation start, or a retained
huge mapping's corresponding extension. Normal cells continue using 64-bit
updates, while only promoted cells pay CX16. A 4,096-entry wide extension array
costs 64 KiB for an arena which ever needs one, plus its pointer/header storage.
That does not imply a measured committed-memory saving.

Such promotion need not wait for an initializer. Each contender can allocate
and fully zero a private candidate, then release-CAS the completed extension
pointer into the arena. A loser uses the winning ready array and disposes its
private candidate. A paused private initializer owns no shared INIT state and
cannot prevent another contender from publishing a complete array. The ready
pointer must be published before the full inline CAS installs the sentinel.
After sentinel publication, nobody may initialize or reset that cell's wide
entry: another publisher or scanner may already be using it.

The sentinel CAS is the promotion point under the exact body scope, after the
payload store. That publisher then invalidates the wide entry before queueing.
If it pauses between promotion and the wide bump, peers can use the already
initialized wide entry; its eventual bump invalidates any intervening scan.
An old inline publisher whose CAS loses must dispatch to the wide path, never
restore inline coverage. Readers observe the inline sentinel, acquire the
persistent pointer, and take a wide snapshot under the same exact lifetime.
Inline and any existing wide entry reset only at the exclusive private
allocation-reuse point, after old body owners are gone and the exact token is
NONE; the arena's extension pointer persists until terminal arena retirement.
Header-only token completion must continue to avoid payload/proof reads. This
reset-on-reuse sketch would require all direct VM TNEW and emitted FNEW resets
to participate; their current inline-only stores cannot reset an extension.
The separate overflow-sidecar audit is considering monotone per-mapping/cell
wide authority and invalidation before sentinel publication to avoid that
reuse-reset dependency. Neither alternative has been implemented here.

Allocation failure is the unresolved part of this alternative. A missing array
cannot turn the sentinel into valid proof, reuse the last inline authority, or
make publishers wait for a particular initializer. Lazy allocation therefore
needs unsuppressible durable recovery plus a retry policy, or storage reserved
before the hot barrier can need it. Keeping the current permanent veto as an
OOM fallback preserves safety but does not prove continued collection under
allocation failure. The allocation operation itself also has no new lockless
progress guarantee. This alternative was assessed, not implemented.

Before accepting that alternative, deterministic controls should pause private
initializers before and after pointer publication, pause promotion before the
wide bump, make an old inline CAS lose to promotion, scan across promotion,
force allocation failure, promote two cells simultaneously, and retain old
small/huge body owners across free/reuse attempts. The era ABA, exact-token
completion, full terminal namespace, cycle identity and missed-descendant
controls must remain. Neither the present prototype nor this design sketch
establishes complete release readiness.

The compact evidence directory is
[gc-table-authority-wide-prototype-2026-09-05](evidence/gc-table-authority-wide-prototype-2026-09-05/).
It contains the exact patch, fixture/adaptor changes, drivers, build and test
commands/results, preserved failures, all paired raw samples, layout records
and complete source/binary SHA-256 identities. Its source manifest stores the
full baseline file list once and exact per-tree overrides, losslessly covering
all six original full manifests. The original trees, executables, archives,
full build logs and disassembly remain under the `/tmp` path above.

The normal baseline executable SHA-256 is
`47445ca81b1e4ef4b6e9c965b076843a63dd7d88743b446fa775c11c9b24dd88`;
the measured wide executable is
`94b089895196c108d0603db804a8ae78f9187ab8a951ad7a650048d80cbdb4f5`.
The exact source-manifest tree identities are respectively
`d6287fed8ac34351b4a4ee62a9d49708c74ff45b775adff7e3a56255ea007baf` and
`7707d00243d759832b7df4d04a53b182114cc53c4d01cc20ca853e6a29585e2c`.
