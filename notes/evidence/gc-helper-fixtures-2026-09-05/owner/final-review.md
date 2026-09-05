The two complete fixture repairs pass all ten final runtime processes on Linux/x64. The unchanged fixtures reproduce their original assertions in six optimized, assertion and ASan controls. The evidence supports repairing fixture setup and stale publication telemetry; this study changes no runtime source or protocol gate.

The frozen delta is `candidate-v4.patch` (SHA256 `9ee974526e9d6a534e8f84da92df0cf19f8f76000f99ab09db7477aad53368ce`). Its inputs are the original root-level tests, and its only outputs are the two files in `candidate-v4/`. The hard-check output is `6584c6cc748f3ae27b93409e24755296d20eaa5ecc1ef6e0576017194021943a`; the allocation-account output is `931232437a4fa22103d3673dcf31f1ad5c2689ebbd70110ce1a743a786f5ddda`. ROOT owns any shared integration and canonical registration.

The hard-check failure is an unfinished MARK cycle with fair close intent. Read-only snapshots in `diag-v3/strict-results.json` show the ordinary-threshold TNEW performs both a hard check and an assist, then leaves cycle 2 in MARK with `mark_close_intent=1`. The best-effort preserving abort returns without completing it. The next `mark_begin` remains in that same cycle; the next hard-only TNEW performs its check, but ordinary worker admission correctly refuses the pending close intent. SMR is open, the native phase gate is open and no worker is active. This reproduction does not support the initial unfinished-SWEEP hypothesis.

The repair completes each previous cycle using the real public full-collection driver, then requires actual IDLE and cleared MARK-close intent. Each arming helper requires IDLE before `mark_begin` and MARK afterward. Every existing assist increment, check increment, batch-cadence exclusion and legacy color-state oracle remains unchanged. `hard-check-proof.md` contains the earlier source proof that ROOT reviewed before the change.

The first allocation-account failure is stale telemetry in a stable generational IDLE interval. The exact unchanged strict case starts at cycle 11 with requests and starts both 11, no pending request, `mark_active=1`, generational mode and minor sweeping enabled. The first meta store filters nine old edges and pushes the parent once. The second filters eleven more and pushes the parent twice. The parent, child and grandchild are already marked after the preceding SSB drain. No cycle or phase transition occurs across either store.

Read-only hardware watchpoints in `diag-v4/publication-watch-results.json` record all twenty increments and their real source stacks. These exact counts are retained as accounting coverage with explicit provenance; they are supplemented by stable phase/old-object preconditions, actual stored table type and identity, the original exact parent push counts, and the original active-SSB parent identity. The later existing interpreter and JIT young-edge tests also retain their mark-before/mark-after-drain requirements and pass as part of every complete final run.

| Filtered publication route | Missing-key store | Existing-key store | Exact 843 source call site |
| --- | ---: | ---: | --- |
| Source value root | 1 | 1 | `lj_meta.c:1008` |
| Captured receiver root | 1 | 1 | `lj_meta.c:627` |
| Keyed-store parent root | 1 | 1 | `lj_tab.c:6366` |
| Keyed-store source root | 1 | 1 | `lj_tab.c:6376` |
| Keyed-store parent/value pair | 1 | 1 | `lj_tab.c:6383` |
| Retained store-guard parent root | 1 | 1 | `lj_gc2.c:16179` |
| Retained store-guard value root | 1 | 1 | `lj_gc2.c:16181` |
| Committed store handoff value | 1 | 1 | `lj_gc2.c:16416` |
| Final meta-store parent/value pair | 1 | 1 | `lj_meta.c:1026` |
| Copied old-value root | 0 | 1 | `lj_tab.c:5241` |
| Copied old-value pair | 0 | 1 | `lj_tab.c:5243` |
| Total | 9 | 11 | Integer keys contribute no GC edge. |

Passing the old counter assertion exposed a second allocation fixture setup failure at the published-SSB assist frontier. `diag-v8/strict-results.json` records parent/child/grandchild as 1/0/0 before the explicit flush. The TG has no free SSB node. The real flush converts two previously published requests to recycle storage; worker SSB conversions rise from 6 to 8, and the frontier becomes 1/1/0 before the measured assist. That assist performs exactly one grey item and reaches 1/1/1. This evidence supports a setup-side advance, not an oversized assist or lost edge. The writer-side recycle is an intended branch in `gc2_recycle_published_ssb_for_flush` and `gc2_flush_ssb`.

The final fixture performs a real full collection **after constructing the second graph**, then proves IDLE and available free SSB storage. It starts a fresh MARK cycle and requires all three objects unmarked and free SSB storage available before marking the parent and flushing. The original required assist run, work accounting, owner cleanup, child=1/grandchild=0 frontier, and eventual grandchild marking after the remaining drain are unchanged. No queue is cleared and no runtime admission state is fabricated by this repair. Existing test-only pacing and ownership controls are unchanged.

Candidate v2 collected before graph construction. Its first free-pool assertion passed, but construction consumed the available storage; its new assertion after `mark_begin` caught the bad setup. That failed generation is retained. Candidate v3 moved collection after construction and passed; v4 additionally makes the stored-value type checks explicit and clarifies comments. `alloc-account-proof.md` is the earlier proposal and remains intact; this document records the final placement and its observed necessity.

| Frozen runtime configuration | Final v4 complete tests | Unchanged original controls |
| --- | ---: | ---: |
| Exact 843, optimized, six helpers, no runtime APICHECK/assert | 2 passed | 2 failed at the original assertions |
| Exact 843, APICHECK/assert and six helpers | 2 passed | 2 failed at the original assertions |
| Exact 843, Clang O1 target ASan, same eight defines | 2 passed | 2 failed at the original assertions |
| Exact 793, APICHECK/assert and six helpers | 2 passed | Not repeated |
| Exact 793, Clang O1 target ASan, same eight defines | 2 passed | Not repeated |

All C fixtures were compiled with assertions enabled, matching their runtime helper defines, `-Wall -Wextra -Werror`, and recorded transitive header hashes. ASan used `detect_leaks=1:abort_on_error=1`. Commands, environment, source/header/archive/binary hashes, stdout, stderr and exit status are in each generation's result JSON. These are complete C fixture processes, including their existing JIT branches; they are not a new broad test sweep or a performance measurement.

`runtime-source-identity.json` verifies all 224 runtime/generator inputs in the original normal/strict/ASan trees equal commit `843786094729ca189b39920cd05ec6756d28444f`. `focused-input-identity.json` additionally verifies every input in the isolated optimized helper build equals 843 and every input in both combined strict/ASan trees equals commit 79345529. Exactly `lj_crecord.c`, `lj_ircall.h`, `lj_tab.c` and `lj_tab.h` differ between these commits. The 793 results are explicitly separate from the 843 study. Build records and binaries are identified in those JSON files. The optimized build copies the parent's actual `ROLLING` version text; its isolated build warning is retained.

ROOT subsequently validated these exact fixtures through their registered suites. Both repaired fixtures pass in the actual m6 GC2-helper-only build. The allocation fixture also links and passes against m10's default build, along with both generational Lua modes. Thus the earlier broad statement in `alloc-account-proof.md` that normal archives cannot link these internal tests was too broad: the allocation fixture enables helper declarations in its own source and its required runtime symbols are available. The hard-check test needs its checkpoint helper. ROOT's separate evidence is `/tmp/lj-helper-fixtures-root-20260905-wa27ui28/canonical.json`. The m6 suite is not a full pass: after these two repaired tests pass, the unchanged `t-jit-idle-reclaim-entry` times out at its original 20-second limit and the remaining two cooperative tests are unrun. ROOT owns that separate diagnosis. These canonical processes are not included in this owner's ten final passes.

Every failed experiment is preserved. `original/` contains the six assertion failures. `diag-v1` contains two compile failures from nonexistent diagnostic accessor names, followed by separate corrected diagnostic generations. `diag-v2` through `diag-v8` contain thirteen failing runtime observations; continuations past stale telemetry were diagnostic only and remain failures. The first GDB observer failed on an optimized-out variable; the corrected observers captured nine and twenty events before their targets aborted. A debugger process returning zero does not make either aborted target a passing test. Candidate v1 passes hard-check but fails the newly exposed assist frontier; candidate v2 fails its stronger setup precondition. Candidate v3's allocation pass and v1's hard-check pass are development evidence, separate from the ten final v4 passes. `releasehelpers-first-failure.json` records the first optimized build-generation setup/recording failure; only the fresh `releasehelpers-v2` build is accepted.

The resulting change is bounded fixture maintenance. The retained exact publication counts intentionally test the current guarded publication route and may need another source-backed revision when that route changes. The new phase and pool preconditions must fail if a later runtime no longer establishes the measured setup. This study establishes neither a general guarantee that preserving abort completes every cycle nor a guarantee that every public collection under arbitrary peers/callbacks returns IDLE. It adds no throughput, concurrent scheduling, Windows or macOS claim.
