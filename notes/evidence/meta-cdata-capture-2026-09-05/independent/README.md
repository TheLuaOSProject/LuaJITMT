# Independent cdata first-method capture validation

2026-09-05. **PASS; no concrete source blocker found.** This is bounded
functional/review evidence, not a performance sample. No shared production/test
file or new runtime test hook was changed by this work.

The reviewed runtime is root's immutable v2 at
`/tmp/lj-meta-cdata-capture-20260905-e3t1w0fw/v2`, base dd2c439179b1e12564710484d8511e4cee617f7f
plus its cdata-capture patch. `lj_meta.c` SHA256 is
`c355b30c7978b31b499b8fe41fff1c03e8ff00f4a2a8293e1e4c74ee3823161e`.
The shared file and frozen normal/assert/ASan copies matched at final review.
`source-manifest.json` pins the patch and 14 relevant identical source copies.

The durable test candidate is `t-meta-cdata-capture.c`, byte-identical to the
validated probe. `durable-fixture.patch` adds it as tests/t-meta-cdata-capture.c
and adds one m5_meta_cdata_capture_protocol registration beside root's two Lua
cases. `m5-registration.lua` and `registration-spec.json` contain the same spec:
compile once with explicit GNU11/-Werror/helper/wrapper flags, then run 13 fresh
processes with a 15-second bound each and restore the default runtime build.
The patch passed git apply --check against the then-current shared suite; it
was not applied by this agent. The exact renamed source also passed all 13 modes
with the canonical flags, and all 13 on ASan with leak detection enabled, without
rebuilding either runtime (`adapted-*-build/results.json`).

## Results

| Execution | Result |
| --- | --- |
| GCC assert fixture, 13 fresh processes | All exit 0; approximately 5–7 ms each; 15-second bounds. |
| Clang target-only ASan fixture, same 13 modes, detect_leaks=0 | All exit 0; no sanitizer report. |
| Root's exact Lua semantic script, ASan, interpreter and JIT modes, detect_leaks=0 | Both exit 0; no sanitizer report. |
| Same unchanged ASan binaries, all 13 modes plus both Lua script modes, detect_leaks=1 | All 15 exit 0; no sanitizer or leak report. C modes approximately 15–18 ms; Lua modes approximately 0.32 seconds. |
| Prior positive-hit archive negative control, basic mode | Expected SIGABRT at the assertion requiring exactly one receiver admission. |

Every functional process uses CPUs 10 and 11; the isolated ASan build uses
CPUs 0–15. Other development work was active on the host. Timing here is only
timeout/progress evidence. The Lua scripts have 60-second bounds. No functional
runtime timeout or unexpected assertion occurred.

`build.json`, `run-*.json/.log`, `asan-probe-build.json`, `asan-run-*`, and
`asan-lsan-run-*` preserve commands and output. Leak-disabled results are retained
separately from the requested leak-enabled repeat. ASan runtime instrumentation
was applied only through TARGET_CFLAGS/TARGET_LDFLAGS in a copied source tree;
host build tools were not instrumented. All C fixture builds retain -Werror.

## Fixture and schedules

`probe.c` creates real cdata with ffi.new("int[1]"), a string key, a cdata base
metatable and a C method through normal APIs. Ordinary padding allocations put
receiver, key, metatable and method in four distinct small arenas. Setup may
stop automatic GC for deterministic geometry; the watched operation always
re-enables GC after a real full collection, and pressure cases start a real MARK
cycle. No phase, queue capacity, allocator cursor, root descriptor or lifetime
field is fabricated.

The fixture enters a real protected C call with authoritative receiver/key/output
stack cells, then calls lj_meta_tgettv_rooted or lj_meta_tsettv_pair directly.
Fixture-only linker wrappers count the actual external lease calls made by
lj_meta.o, track its extra lease tokens and reject table waits with outstanding
scopes, blocking SMR entry and VM/protected-call reentry during that helper call.
The object relocations are recorded in `meta-wrapper-relocations.txt`.

On helper return, it requires exact receiver/key/function words in the prepared
ordinary metamethod frame and unchanged aliased input/output words. It transfers
that frame's function into an enumerated output stack root before discarding
the fixture's unused frame, deletes source edges, collects fully to IDLE and
invokes the retained method through a real protected call. The callback asserts
zero leases/SMR/extra scopes/chain anchors, checks its arguments and collects
again. This explicitly tests the helper handoff; root's separate Lua script
tests immediate normal VM/CAPI dispatch and actual FFI callbacks.

| Mode | Required observation |
| --- | --- |
| basic | One receiver, key, metatable and method admission, zero waits; callback returns 11 after deleting old edges and full GC. |
| alias-source / alias-key | Explicit output aliases receiver or key. The function path leaves those original words unchanged and prepares the correct frame. |
| same-source-key | Receiver and key are the same actual cdata cell. The two input obligations account for two source admissions, with one metatable/method admission each. |
| set-alias | Paired setter receives RHS==receiver. It prepares the same exact method/receiver/key frame; caller-style RHS materialization and callback see the original cdata. |
| retry-source / retry-key | Existing one-shot admission refusal is consumed exactly once. Mandatory capture retries after releasing all scopes; one table wait is observed with zero readers. |
| retry-mt / retry-method | Optional capture refuses after releasing its extra scope(s), and ordinary lookup succeeds. Counts are respectively 2/1/2/1 and 2/1/2/2; no optional-scope wait or reentry occurs. |
| replace | Both existing metatable pause hooks execute. Source SMR and receiver/key leases are present before old target admission. The observer replaces the cdata base root, then observes an exact lease on the captured old metatable and replaces its method. Lookup returns the old table's newly observed method (33), not the replacement base table's method (22). An attempted IDLE reclaim is refused while source SMR is held. |
| growth | Full real grey and SSB queues force normal queue allocation during method-root publication; detailed retained-scope evidence below. |
| fail-growth | Existing grey-grow failure helper forces the same queue allocation failure branch; the published identity enters allocation-side recovery and all extra leases subsequently release. |
| throw | The retained real C method collects, then raises a catchable Lua error. The protected call catches it after all helper scopes and chain anchors have closed. |

The replacement observer does not attach a TG, allocate, run a collector or
append to a foreign owner SSB. It performs bounded atomic replacement of already
rooted global/table edges at IDLE using stable prepared storage. The pressure
observer performs only edge deletion after the method publication begins.

## Queue publication evidence

Pressure setup uses existing enqueue helpers to fill the real 256-entry embedded
grey deque with graph-bearing filler identities, fill/flush one 1,024-slot SSB,
then fill the other. There is no free SSB node. The method is required white in
the new MARK cycle, and all four arena reader counts start at zero.

At LJ_GC2_RECOVERY_TEST_SSB_COMMITTED, after the first filler identity commits and
its temporary mark scope releases, both pressure modes require:

- Receiver/key/metatable/method arena readers are exactly **1/1/1/2**. Method has
  the copied-result lease and the publication marker lease. The two extra
  meta-level tokens are still owned.
- The four chain anchors exist and receiver/key/method words match exactly.
  Source/vector SMR is **0**, worker_active and ssb_consumer_active are **1**.
- The source SSB slot/count is still present, the active node is still full, and
  the method's mark is set. No table wait, blocking SMR, VM entry or callback has
  executed inside the watched helper.
- On successful growth, capacity is **512**, logical bytes increased by exactly
  **4096**, and recovery items remain **0**.
- On forced grow failure, capacity remains **256**, logical bytes increased by
  **0**, and exactly **1** allocation-side recovery identity exists.

The observer then removes the cdata base metatable root and the original method
table edge, leaving the private method anchor and retained scopes as the precise
handoff. The helper resumes, returns the ordinary complete call frame and has
zero remaining scopes. After transfer to an enumerated output and full GC, the
same method remains callable; the recovery count is back to zero.

The failure hook returns before invoking the allocator. This proves the
queue-failure/recovery cleanup branch, **not genuine OS/allocator OOM**. The
separate source review establishes why an actual allocator NULL follows this
same nonthrowing return path under the current default policy. A successful grow
alone would not have supplied the failure evidence.

## Complementary Lua and negative controls

`t-meta-cdata-capture.lua` is copied exactly from root's v2/lua-semantics script,
SHA256 `0774a146476acbf4887481f18c8b82a4b5674d7ab0592dd1105a75cc5e41eb14`.
Its interpreter and JIT ASan runs cover small/aligned/Huge cdata, real Lua
metamethod collection/reentry/throw, an ffi.cast C callback, table-valued chains,
sequential base/method replacement/absence/invalid values and retained results.
No duplicate semantic script was authored here.

The negative executable uses the **older rooted-positive-hit assert archive**
at /tmp/lj-rooted-positive-hit-20260905-34e9qs5t/assert/src/libluajit.a, SHA256
`9695d469b23d70e5ff4740a635a66c7997445d1703afff0e58e78b42e6a36589`.
It predates the later tag guard/FNEW repair as well as optional cdata capture.
It is **not an exact one-diff runtime control or a performance comparison**.
Its intended negative capability is narrow: the old meta lookup takes a second
receiver admission, and the fixture rejects that at probe.c:374 rather than
accepting a semantic-only fallback as proof of the optimization.

One fixture compile setup failure is retained as
`asan-probe-build-c99-rejected.*`: Clang -std=c99 -Werror rejected existing repeated
typedef declarations in runtime headers. The final fixture uses -std=gnu11, as
the prior ASan fixture builds did; no warning was demoted and no runtime source
changed. The main target-only ASan build succeeded on its first attempt.

## Limits

This is a small deterministic schedule inventory, not exhaustive concurrency
exploration. Exact pressure counts use small cdata, small vectors and a C method;
the Lua script supplies complementary Huge/aligned/Lua/FFI semantics without the
same paused queue schedule. The observer is not a concurrently running GC worker
or mutator TG. Leaked protocol scopes are checked through explicit counters as
well as ASan/LSan; arena-internal lifetime errors are not all equivalent to host
heap errors visible to sanitizers. No TSan or actual allocator-OOM run was added.
Named wrappers guard relocation-based entry points, not every inlined operation.
`review.md` records the source-side authority and exception reasoning.
