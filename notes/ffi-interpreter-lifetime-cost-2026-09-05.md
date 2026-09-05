# Interpreter FFI lifetime cost and bounded admission reuse review

2026-09-05. Diagnosis against the frozen normal rooted-positive-hit build
corresponding to the production change later committed as 28de50a6. Subsequent
source review uses that same metamethod implementation. No production/test
change or additional benchmark was made for this note.

The interpreter FFI struct loop progresses linearly but costs roughly **20x
stock**. The dominant work is repeated lifetime admission in generic
metamethod dispatch. The small acquire tag guard that avoids table attempts
on cdata addresses a separate **3.3% candidate/control difference**; it does
not resolve the preexisting generic metamethod cost.

[Durable text evidence](../bench/ffi-interpreter-lifetime-cost-2026-09-05/README.md)
includes exact commands, binaries/harness hashes, raw normal output and a
separate profile. The [original diagnosis](../bench/ffi-interpreter-lifetime-cost-2026-09-05/diagnosis-original.md)
is copied without revision.

## Recorded measurements and later full completion

All nine normal samples used the unmodified filtered harness, JIT off, GC
enabled and BENCH_GC_MODE unset on CPU 29. Each reported row is the minimum of
five in-process rounds; these are limited diagnostic samples, not a release
performance distribution.

| Iterations per round | Stock ns/iteration | Prior integrated control | Rooted-hit candidate |
| ---: | ---: | ---: | ---: |
| 3,000 | 79.00 | 1,533.00 | 1,579.67 |
| 30,000 | 79.27 | 1,513.10 | 1,566.20 |
| 300,000 | 77.22 | 1,515.80 | 1,566.31 |

Every process exited 0 within its 10- or 15-second bound. At 300,000 iterations,
candidate/control is 1.03332 and candidate/stock is 20.28373. Other work occupied
CPUs 30 and 31; neither host activity nor frequency was fully isolated.

The original diagnosis saw only the FFI row from root's still-running extended
full interpreter observation: 47.6172 seconds / 1,587.24 ns per iteration for
30 million iterations. Root's later
[completion record](../bench/ffi-interpreter-lifetime-cost-2026-09-05/extended-interpreter/result.json)
now confirms **exit 0, all 15 rows, 727.427096 seconds**, under a 900-second
bound. The complete stdout and command/environment metadata are copied beside
that record. The process was never attached to, instrumented or signaled by
this diagnosis. Its FFI row is about 1.3% above the filtered result, supporting
linear progress rather than a demonstrated full-sequence FFI stall. The
earlier 360-second timeout remains a separate observation.

A separate 300,000-iteration candidate perf run collected 1,462 samples, none
lost. Matching symbols came from a local relink of the frozen objects: same
Build ID and exact .text bytes, original CLI unchanged. The text reports show
62.12% cumulative cost under meta_tget_rooted_mode and 28.80% under the rooted
setter; 50.37% under exact TValue lease acquisition. Actual FFI __index and
__newindex handlers account for 3.50% and 1.92% cumulative cost. These shares
overlap and are not independent additive costs.

The loop reads two fields and writes one per iteration. Initial capture
leases receiver and key. Metamethod lookup then leases that receiver again,
plus its current base metatable and the copied method function. Small-arena
registry searches and counted reader entry/release dominate the self samples.
Root-anchor initialization is only 1.30% cumulative, so removing anchors or
rewriting CType field lookup is not justified by this profile.

## Smallest bounded follow-up design

**Design only; no implementation or validation result is claimed.** Keep the
existing four chain anchors, publication barriers, Lua/C/FFI method dispatch,
call-frame setup and ordinary fallback. Reuse the receiver admission already
held during initial capture for one opportunistic function-valued cdata
metamethod lookup.

One narrow implementation shape is an optional first-method capture inside
[meta_chain_capture_inputs](../src/lj_meta.c), after its successful receiver/key
admission and before it drops source SMR:

1. Reserve/push all chain anchors through the existing initializer before any
   lease. Initialize a caller-local first-method-ready flag to false. Exclude
   function-environment mode. Enter the usual source SMR before loading the
   original authoritative receiver/key roots; preserve stack offsets and the
   existing retry/stale-input handling.
2. After both input leases succeed, try the extra work only when the admitted
   receiver is cdata. Load its current base metatable with lj_basemt_obj_acq.
   The source SMR precedes that replaceable global-root load. Admit that exact
   table with lj_gc2_tv_lease_acquire.
3. Preserve both existing test pauses: meta_test_pause_after_mt_capture after
   the fresh edge load, and meta_test_pause_after_mt_lease after successful
   exact table admission. In the same retaining interval, use the bounded
   lj_tab_getstr_held_try for __index or __newindex. It keeps the existing vector
   generation/retirement checks and rejects locked keys, forwarding and
   publication claims. On a function-tagged result, acquire its exact method
   lease before any header use or SMR leave. Never call a looping/waiting table
   getter here.
4. On success, copy the original receiver/key snapshots and exact method into
   their existing anchors. Discard every vector pointer, leave source SMR, and
   publish the method anchor while all four leases remain held. Release the
   method and metatable leases, then perform the existing receiver/key root
   publications under their input leases and release those leases. Return
   first-method-ready only after that complete handoff. This ordering confines
   the extra leases to the function-specific publication route.
5. The get/set caller consumes that flag only for the immediate first cdata
   hop. It skips the otherwise immediate lj_meta_lookuptv call and uses the
   existing tvisfunc/method-call branch. The flag is not a cache and cannot
   carry into a later table-valued chain hop.
6. If the optional metatable/method attempt is absent, nonfunction, stale or
   transiently refused, release its extra leases and complete the ordinary
   receiver/key capture with the flag false. Leave the method anchor
   unpublished. Existing lookup, retry, errors and chain semantics then run.
   Failure of mandatory receiver/key capture keeps today's behavior.

This retains every existing counted body validator, metatable/method admission
and semantic publication barrier. On the common successful first hop it
removes **one duplicate receiver admission and two separate outer SMR
intervals**: current capture uses one interval and lj_meta_lookuptv uses two;
the combined attempt uses the capture interval. No new persistent cache,
allocator/TG field, exposed lease API or lease ownership across caller returns
is needed. The existing standalone lj_meta_lookuptv can remain unchanged.

Keeping the lease work inside capture is preferable to returning a live lease
to both get/set callers: those callers have multiple retries, errors, chain
branches and callback exits. A successful first-method flag contains only
already-published anchor authority and creates no new cleanup obligation there.
No speedup is predicted from sample shares; an implementation needs a separate
matched measurement.

## Authority and escape boundaries

| Boundary | Required invariant |
| --- | --- |
| Original receiver/key | Load their actual authoritative source cells after source SMR starts. A preflight tag or C-local copy never replaces these roots. Copy to owner-private anchors only under exact leases. |
| Base metatable replacement | The cdata base metatable is replaceable through lua_setmetatable; do not treat the initial FFI table as immutable. Load the edge afresh under SMR and lease the exact captured allocation. |
| Method replacement/resize | Keep the existing bounded generation-aware table lookup and method lease before leaving vector SMR. An old or new observed edge has the same retained authority as today's lookup; there is no cached method across calls. |
| Aliased output/source/key | The attempt writes only its newly reserved private anchors. Existing output and method-call code consumes the captured pair; output aliases remain untouched until their current terminal publication. |
| Setter RHS aliases | Leave value publication and stack-offset restoration in meta_tsettv_pair_mode unchanged. Do not capture or overwrite its RHS as part of the optional method lookup. |
| Real Lua methods and FFI callbacks | Accept the exact function selected from the current metatable. Never substitute a known FFI entry point or call a C address directly. Existing dispatch may later select a user metatype method, allocate, wait or throw. |
| Wait/throw/allocation | Anchor reservation and ordinary allocating/error paths occur before admission or after complete release. No extra lease survives return, fallback, parse wait, table wait, call-frame setup or invocation. |
| Root publication | Physical admission is not semantic marking. Keep the ordinary barriers before releasing the exact scopes. Their existing nonthrowing queue growth can occur after vector SMR closes; never release early merely to claim allocation-free publication. |

Relevant source boundaries are input capture in lj_meta.c:506, ordinary
metamethod lookup at :194, method-frame publication at :683 and initial
get/set dispatch at :694/:798; table held lookup in lj_tab.c:3990; exact
admission in lj_gc2.c:17215; base-root loads/stores in lj_obj.h:6485 and
lua_setmetatable's base-root replacement in lj_api.c:2647. The source manifest
in the evidence identifies the exact frozen versions; later edits may move
line numbers.

## Publication allocation and failure proof

The [source audit](../bench/ffi-interpreter-lifetime-cost-2026-09-05/publication-source-audit.md)
records the call chain and exact source hashes separately from measurements.
Under the current default internal-allocator policy, function-root publication
can grow the grey queue without raising a Lua error: gc2_grey_grow calls
lj_mem_new_nothrow, which returns NULL on failure. The queue publisher retains
an allocation-side recovery identity or installs NO_RECLAIM and returns; it
does not call lj_err_mem. Successful growth may account enough bytes to attempt
GC assist, but writer-side SSB conversion already owns the worker token, so
that nested assist cannot acquire it or invoke arbitrary GC work. Neither the
function-root path nor this conversion invokes the captured method.

This proof depends on LJ_GC2_INTERNAL_ALLOCATOR_ONLY=1. lua_newstate forces the
arena allocator and lua_setallocf is a no-op under that policy. The dormant
custom-allocator branch calls a callback directly, so a function named
"nothrow" is not by itself evidence against callback longjmp. The optional
capture should compile to refusal unless LJ_HASFFI and the internal-allocator
policy are enabled. Existing lookup remains the fallback in other builds.

Publish/release the extra method and metatable scopes before the original
receiver/key barriers. An arbitrary table-valued key can reach an existing
immediate table traversal after failed rescan publication; retaining extra
leases across that broader legacy path is unnecessary for this change. This
review does not claim a complete throw/ownership audit of that rare fallback.

OOM on the reviewed function queue-growth path returns to one unconditional
cleanup tail. Every successfully acquired extra lease must be released there;
on optional refusal, release only the acquired metatable/method scopes before
ordinary input publication. Failed lease acquisition already leaves no owned
token. Source SMR must close before any root barrier. Anchor allocation still
occurs before admission, and table waits, frame allocation and method calls
occur only after all scopes close. Fatal invariant aborts are process
termination, not catchable exceptions with a cleanup guarantee.

The earlier full-SSB probe demonstrated successful queue growth under held
source/result leases. It did **not** inject OOM. No new allocation-failure test
has been run for this design; source proof and the proposed failure fixture
must remain distinct evidence.

## Minimum validation before adoption

These are proposed checks, not work performed for this design.

- Count successful receiver admissions and outer SMR intervals on both cdata
  field reads and writes. Confirm the reduction with the same public semantics,
  not a cached callback or omitted publication.
- Reuse the metatable-capture/lease pause schedule, extended to this optional
  path: replace/remove the cdata base metatable, resize/replace its method,
  delete old edges and collect while retaining the chosen incarnation.
- Force receiver, key, metatable and method admission refusal independently;
  include table publication claims/retiring vectors and SMR denial. Fallback
  must retain normal results/errors with zero leaked leases, SMR readers or
  chain anchors, and no wait while any extra scope remains held.
- Exercise real Lua __index/__newindex functions that collect, allocate,
  reenter and throw; ordinary C/FFI callbacks; and table-valued, absent and
  invalid methods. Preserve function-environment exclusion and later chain
  hops. Existing t-meta-rooted-chain coverage is a base, not proof of the new
  path.
- Cover output==receiver, output==key, shared receiver/key cells and setter
  RHS aliases, including C API and VM callers. Check frame/root publication
  before the first callback and exact cleanup after caught exceptions.
- Force root-anchor allocation failure before admission, full SSB/real queue
  growth during method publication, and the existing grey-grow failure helper
  with recovery fallback. Distinguish that deterministic failure branch from
  genuine allocator OOM if the latter is not exercised. At publication require exact
  source/key/metatable/method retention, no vector SMR and no catchable
  reentry; after return/invocation require complete scope cleanup.
- Run small/interior/Huge cdata and metatable vector geometry plus relevant
  GC phase/late-recovery controls. Keep the same exact extent/type checks.
  Measure this design separately from the tag guard and from stock.

The bounded review found no need for a broader FFI rewrite. It identifies a
specific duplicated admission and a contained way to reuse it; replacement,
publication and callback proof obligations remain prerequisites to shipping.
