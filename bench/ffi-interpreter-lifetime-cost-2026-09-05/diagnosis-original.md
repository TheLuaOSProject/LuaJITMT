# Interpreter FFI struct diagnosis

2026-09-05. Bounded independent diagnosis on CPU 29; no shared runtime or
documentation edits and no attachment to root's running full benchmark.

**Conclusion:** the interpreter FFI loop progresses linearly at approximately
1.5 microseconds per iteration in both frozen fork runtimes, around 20 times
stock. Most cost is repeated generic metamethod lifetime/provenance admission,
not C struct field layout or conversion. The candidate's extra positive table
attempt on a cdata receiver adds approximately 3% to the existing control cost.
An acquire tag-only guard can avoid both table attempts for non-table receivers
without weakening the exact helper or fallback contracts.

## Normal timings, separate from profiling

Every normal sample is a fresh process running the unmodified frozen harness,
filtered to ffi_struct, with JIT off and GC enabled. BENCH_GC_MODE is unset.
The harness returns the minimum of five in-process rounds, so these rows are
not five independent process repetitions. Setup still builds the harness's
preexisting 8,192-key table. No stopped-GC control was used.

| Iterations per round | Stock ns/iteration | Integrated control | Rooted-hit candidate |
| ---: | ---: | ---: | ---: |
| 3,000 | 79.00 | 1,533.00 | 1,579.67 |
| 30,000 | 79.27 | 1,513.10 | 1,566.20 |
| 300,000 | 77.22 | 1,515.80 | 1,566.31 |

All nine processes exited 0. Each had a 10- or 15-second timeout; the largest
candidate/control process completed in less than 3 seconds. At 300,000
iterations, candidate/control is 1.03332 and candidate/stock is 20.28373.
These are limited diagnostic samples; CPU 30 ran root's full interpreter
benchmark and CPU 31 ran another study, without host/frequency isolation.

During this diagnosis, the untouched full interpreter stdout at
 /tmp/lj-rooted-hit-full-benchmark-20260905-96tl6pn9/extended-interpreter/fork-joff.stdout
reported ffi_struct 47.6172 seconds, 1,587.24 ns/iteration at 30 million
iterations. That is about 1.3% above the filtered 300,000-iteration candidate
result and supports linear progress. Five rounds at approximately that cost
explain a long delay before the row prints. This records that one row only;
it does not claim the entire full benchmark completed. No further prefix
experiment was run.

The control is the frozen integrated build at
 /tmp/lj-linux-integrated-stability-20260905-st7b2_hh/normal
and the candidate is
 /tmp/lj-rooted-positive-hit-20260905-34e9qs5t/normal
Stock is the pinned build at
 /tmp/lj-runtime-performance-review-2026-09-04/stock
Exact binary/harness identities and commands are recorded in metadata.json,
tiny-runs.json and scaling-runs.json.

## Independent profile

A separate candidate process ran the same unmodified filtered harness at
300,000 iterations under perf cycles:u, 499 Hz, DWARF call graphs. It completed
in 3.017 seconds. Perf recorded 1,462 samples and no lost samples. Instrumented
times are not used in the normal comparison above.

The supplied CLI is stripped. A symbols-only executable was linked under /tmp
from its existing luajit.o and libluajit.a, without rebuilding any runtime
source. Its .text SHA-256 exactly matches the frozen executable:
 729c8af8d5bb232648e8a808377d504d55bd9faab76c095afb0c7656ddaf0805
and both Build IDs are:
 55bde989ba3fa1c02974d48914c2a33b9f97206d
The frozen executable SHA-256 was checked unchanged. Perf used an isolated
symbol filesystem pointing at this matching executable; measurements always
used the frozen CLI.

Key cumulative shares (overlap and must not be added indiscriminately):

| Path | Cumulative cycles |
| --- | ---: |
| meta_tget_rooted_mode | 62.12% |
| meta_tset_rooted_mode | 28.80% |
| lj_gc2_tv_lease_acquire | 50.37% |
| lj_meta_lookuptv | 38.23% |
| meta_chain_capture_inputs | 33.58% |
| lj_gc_pubroot | 17.38% |
| lj_tab_gettv_rooted_hit_try | 3.09% |
| lj_cf_ffi_meta___index | 3.50% |
| lj_cf_ffi_meta___newindex | 1.92% |
| lj_ctype_getfieldq_wait | 1.86% |
| meta_chain_roots_init | 1.30% |

Leading self shares are hugetab_reader_entry 11.43%, counted lease release
10.55%, gc2_small_candidate_admit 10.49%, hugetab_search 9.21%,
arena_publish_leave 5.83% and lj_arena_rescue_enter 5.15%. These functions also
manage small-arena registry admission; their names do not mean the tiny point
struct is a huge allocation.

Per iteration the Lua loop performs two cdata field reads and one field write.
Those enter generic rooted metamethod handling. Input capture leases the cdata
receiver and string key, publishes anchors, then releases them. Metamethod
lookup re-admits the receiver, admits the current base metatable and the copied
FFI handler function, and transfers that method to the caller's root. Call
setup then publishes its arguments. The repeated registry lookups, counted
reader transitions and exact cdata geometry validation dominate this profile.
The actual CType field lookup and numeric conversion are small shares; this
does not justify a broad FFI implementation rewrite or removing lifetime
checks.

## Concrete small guard and safety review

Frozen lj_meta.c:694 calls both positive table helpers for every ordinary
receiver, including cdata. The scalar helper refuses a non-table. The broader
tab_gettv_rooted_try_impl at lj_tab.c:4862 obtains source SMR before loading the
receiver and discovering it is not a table. That unnecessary attempt accounts
for 3.09% cumulative samples and is consistent with the small candidate/control
difference.

Suggested shape, for root to implement and measure separately:

~~~c
if (!funcenv) {
  TValue hint;
  lj_tv_load_acq(&hint, o);
  if (tvistab(&hint) &&
      (lj_tab_getscalar_rooted_try(L, o, k, out) ||
       lj_tab_gettv_rooted_hit_try(L, o, k, out)))
    return out;
}
~~~

No correctness blocker was found for this guard under the existing internal
meta caller contract:

- The receiver source cell is authoritative and remains valid for the current
  owned call. An acquire atomic TValue load used only to inspect tag bits does
  not dereference a GC body and needs no body lifetime lease or source SMR.
- The hint is never passed as a replacement root or dereferenced. Both helpers
  still receive the original authoritative receiver/key cells and retain all
  existing exact source, owner, generation, lease and output checks.
- A table-to-non-table change is re-read and handled by the exact helper.
  A non-table-to-table change skips the optimization and reaches the existing
  captured chain, which reloads and admits the source in its ordinary SMR
  interval. Neither path invents stale pointer authority.
- The guard performs no output store, allocation, wait or callback, so output
  aliasing either input keeps its existing semantics.
- Function-environment mode remains excluded: the function root's tag is not
  the replaceable environment table edge. Its original capture path is needed.
- Stable table roots still reach the scalar helper even when global SMR is
  closed, preserving that helper's independent small-lease capability.

This guard targets the measured additional few percent, not the preexisting
roughly 20x interpreter gap. Any future reduction of repeated metamethod
admission needs a separate design for retaining source/key/metatable/method
authority through call-frame publication, exact replacement/reuse behavior and
release before allocation/wait/throw. No such broader change is proposed or
implemented by this diagnosis.

Relevant frozen source:
- lj_meta.c:466 / 506: root reservation and retained input capture.
- lj_meta.c:194: metamethod lookup and exact receiver/metatable/result leases.
- lj_meta.c:683 / 694: metamethod call setup and ordinary get dispatch.
- lj_meta.c:798: generic rooted write dispatch.
- lj_tab.c:4862: broader rooted table attempt opens SMR before tag refusal.
- lj_gc2.c:17215: exact small-object admission and cdata geometry validation.
- lj_arena.c:3500 / 3660: counted registry-reader entry and exact reader lookup.
- lib_ffi.c:1160 / 1179: FFI __index / __newindex handlers.
- lj_cdata.c:780 / 969 / 1012: field resolution, get and set.

## Evidence inventory and limits

metadata.json pins the three normal runtimes and exact unmodified harness.
The nine normal stdout/stderr files and JSON commands retain every result.
candidate-filtered-profile.json records the separate instrumented process;
symbolized flat/callgraph reports and original perf data remain under /tmp.
symbol-link.json and symbol-reports.json preserve symbol provenance commands.
summary.json and manifest.json pin the final compact summary and artifacts.

The profile includes harness initialization and explicit inter-round GC; it is
not a target-only instruction counter. Native call-chain attribution around VM
assembly is approximate. No concurrent cdef, receiver replacement, forced SMR
denial or OOM was injected. Guard safety is source review, not a claimed test
of an unimplemented patch. Root's full benchmark was only observed through its
stdout, never instrumented or signaled.

