# Positive rooted table hits before metamethod-chain allocation

Ordinary positive table reads now reuse the existing bounded rooted reader
after the non-SMR scalar attempt, before allocating metamethod-chain anchors.
This removes the extra chain setup for GC-valued results and Huge vectors.
Small scalar reads retain their ability to complete while the real unrelated
metadata reclaimer is paused. The general reader still requires SMR admission;
this change does not make all ordinary reads nonblocking.

## Contract and source review

`lj_tab_gettv_rooted_hit_try` shares the existing tri-state reader's complete
table/key/result lease, paired-vector, source confirmation, and output
publication sequence. Its only different behavior is to leave every input and
output word unchanged on absence or refusal. This includes output aliases.
The older tri-state APIs keep their existing nil-on-failure contract.

Meta callers can therefore try a positive read before capturing their general
chain roots. A miss or failed admission falls through with the original
operands intact. Function-environment lookup remains on its existing capture
path. No new allocation, callback, or retry loop is added to the read interval.

Independent review checked all relevant caller provenance: owned VM stack,
TGETS temporary backed by the active prototype's immutable constant, C API
environment roots materialized in the stack, and the FFI metatype root
transferred into `base`. An actor-owner snapshot alone is not the provenance
proof. Source SMR starts before snapshots; exact object leases survive through
destination publication after the vector interval ends.

The inherited assertion that the rooted reader never allocates was too strong.
Successful GC-result publication can fill an SSB and grow the existing grey
queue after vector SMR closes, while the exact object leases remain held.
The header now describes one-shot admission and no chain-anchor allocation.
Queue allocation, the general SMR gate, and the wider runtime progress debt
are not eliminated by this change.

## Validation

The frozen build base is `5411209f`, with three production files and the
rooted-get fixture overlaid. The normal executable SHA-256 is
`cfd0a47e14be6de5f1a3fde057abb88752ee7aeb516990b2884ae77acf01db17`.
The final shared header additionally corrects documentation only; the measured
header, final header, and exact text delta are preserved separately. All
compiled declarations and production function bodies are unchanged by that
documentation correction.

Eight strict/assert and ASan C runs pass: the extended rooted-get fixture,
paused-reclaimer scalar fixture, rooted length, and rooted reader. The new
checks require unchanged miss/refusal operands, both output aliases, table/
key/result admission refusal, Huge vectors, retired-vector refusal, unchanged
ordinary-hit accounting, and a returned function surviving collection after
its source edge is deleted. A negative variant changes only the positive
entry's output policy back to the old nil-on-failure behavior; it fails the
exact untouched-output assertion under SMR refusal.

Normal stock suites pass 387 interpreter and 509 JIT cases. The 15 general
resize cases, three native resize cases, weak/finalizer JIT overlap, remote
native stack GC, and rooted-reader Lua stress pass. Canonical metamethod-chain
and x64 rooted-read suites also pass. The first custom driver used an
unrecognized case-selection variable and inadvertently ran native coverage
with JIT off; that failure and the original driver remain archived. The
corrected driver selects and records the actual intended cases.

An independent real queue-pressure probe fills both 1,024-slot SSB buffers and
the 256-entry grey queue before the hit. During conversion the grey queue grows
to 512 entries while distinct source/key/result arenas hold exactly 1/1/2
readers, SMR readers are zero, and the output/root owner remains exact. On
return all scopes are released, the result is in the new active SSB, and grey
capacity has reached 2,048 with a net 16,384 accounted bytes. Removing the
source edge, completing GC to IDLE, and invoking the result returns 12345.
There are no table waits, blocking SMR calls, or Lua callbacks during the hit.
This is successful growth coverage; it does not claim queue OOM or forced
nested hard-assist validation. Failed initial probe setups are preserved.

Runtime ASan uses `detect_leaks=1:abort_on_error=1` without suppressions;
generator/build-only execution disables leak detection. The pressure probe
uses the strict archive. Functional evidence, source review, exact inputs,
commands, raw logs, negative control, and final documentation delta are in
`notes/evidence/meta-positive-rooted-hit-2026-09-05/`.

## Matched normal measurements

Seven alternating fresh-process pairs per case compare the committed final
arena/scalar runtime with this candidate. Every tracked runtime input matches
except `lj_meta.c`, `lj_tab.c`, and `lj_tab.h`. Both normal static builds execute
the unmodified filtered harness with JIT off, `BENCH_SCALE=0.005`, its permanent
8,192-key graph, GC enabled, and five internal rounds. All 56 processes exit 0
within their 30-second bounds. CPU 30 is pinned; other functional work uses
CPUs 0–15 on the shared host. Final executable hashes match.

| Case | Before median ns/op | After median ns/op | Paired geometric ratio |
| --- | ---: | ---: | ---: |
| Existing-key stores | 734.42 | 273.96 | 0.373 |
| Hash reads | 572.31 | 404.27 | 0.706 |
| Existing-key reads | 1,275.91 | 627.31 | 0.493 |
| Array read/write | 168.37 | 168.51 | 1.005 |

The store workload reads GC-valued keys from a table, so cheaper reads also
reduce its measured cost. These comparisons repair the previously measured
fallback regressions and preserve the small scalar array benefit. They are
filtered measurements, not stock parity or a full-suite aggregate. Commands,
source identities, raw pairs and normal build metadata are in
`bench/meta-positive-rooted-hit-2026-09-05/`.

The separate full JIT pilot completes 15 rows with a 1.337883709 fork/stock
geometric mean. Its interpreter run reaches 12 rows before the 360-second
limit; that original incomplete result is preserved. A separate full
interpreter process with a 900-second bound completes all 15 rows in 727.427
seconds. Compared with the earlier stock interpreter sample, its geometric
ratio is 9.694639005. Exact bounds, raw outputs, comparison limits and remaining
costs are recorded in `notes/linux-rooted-hit-full-performance-2026-09-05.md`
and its durable benchmark artifacts. Neither extending the bound nor completing
more rows alone establishes a speedup.
