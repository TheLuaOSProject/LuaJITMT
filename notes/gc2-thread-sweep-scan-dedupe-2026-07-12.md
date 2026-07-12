# GC2 SWEEP thread-scan deduplication

## Failure

`m4_threading_api` stopped at the first explicit collection while a dropped
thread handle remained live and its worker was parked in an untimed channel
receive. The worker continued leaving its one-millisecond native wait and
acknowledging safepoints, but the main thread performed hundreds of thousands
of SWEEP root scans without reaching the bridge.

The representative wait predicates were:

- main: `collectgarbage("collect")` in `lj_gc2_trace_sweep_roots()`, either
  waiting for a `SCAN_ROOTS|FLUSH_SSB` acknowledgement or draining the work
  published by that acknowledgement;
- child: `chan_wait()` in a one-millisecond futex wait, with the native boundary
  periodically acknowledging the request;
- `thread_scan_needscan_pending` repeatedly returned to zero, so this was not a
  lost acknowledgement or permanently stranded handoff;
- `major_root_scans` and `thread_scan_needscan` grew together into the hundreds
  of thousands while SSB/grey work was repeatedly produced and drained.

## Thread identity cause and fix

An owner acknowledgement scans the parked thread synchronously and publishes
the current GC cycle, stack-dirty epoch, and handoff epoch. The later global
thread-registry pass nevertheless routed the same already-covered thread
through `lj_gc2_trace_sweep_root()`. That function preserved the body and
unconditionally queued every traversable object during SWEEP. Traversal of the
redundant thread item consumed the handoff stamp or manufactured another
`NEEDSCAN`, which requested another all-TG root snapshot and repeated forever.

After `gc2_markobj_preserve_status()` has retained the thread body, a current
thread scan stamp is complete payload proof. `lj_gc2_trace_sweep_root()` now
returns without publishing duplicate SSB work when
`gc2_thread_scan_current()` succeeds. A stale dirty epoch, missing cycle stamp,
or live `NEEDSCAN` still takes the existing publication/handoff path.

The self-owner classification also excludes the current TLS TG explicitly.
The existing `L != lj_tg_cur_L(g)` comparison normally supplies the same
answer, but collection entered from a secondary TG must not depend on fallback
current-state resolution while its exact TLS owner is available. The current
owner can publish an authoritative stack scan; only a different live TG is a
remote-current owner which needs the handoff protocol.

## Post-bridge peer SSB cause and fix

Once the thread loop was removed, secondary-thread explicit collection exposed
a second close cycle. `gc2_worker_drain_inner()` rotates the logical
collector's SSB before taking `worker_active`, but a different TG can publish a
below-capacity private suffix after the SWEEP bridge snapshot. In the concrete
fixture, the main TG was native-parked in the timed reply receive and retained
29 active SSB references while the child collector had no active, published,
grey, recovery, or NEEDSCAN work.

The complete empty predicate correctly rejected that private suffix, but the
only remaining `FLUSH_SSB` handshake was after the first `sweep_to_idle`
predicate. This was circular: the suffix prevented entry to the handshake
which could rotate it.

After a zero-progress SWEEP drain proves that published SSB, grey, and recovery
work are empty, the worker now distinguishes a peer-private suffix and performs
one `FLUSH_SSB` handshake. It reports one scheduling unit so the next bounded
pass drains the newly published node. The current logical TG was already
rotated before the worker claim; the handshake is for other native/VM owners.
The ordinary close predicate and final revalidation remain unchanged.

## Tactical table-body SMR collision

With peer SSB work making progress, a rare main-mutator publication could pin
the typed activation to `NO_RECLAIM` while the child owned a physical sweep
batch. GDB initially attributed the merged optimized call site to HugeTab
reader saturation. The machine-code predecessor disambiguated it:

- the HugeTab overflow path overwrites `r12d` with `marked` (`4` or `-2`);
- the trapped call retained `r12 = 0x7ffff7a6b3d0`, the raw table-node header;
- that predecessor is the failed `lj_gc2_smr_read_try()` branch;
- dumps of the only two TG HugeTabs contained no `0xffff` reader count.

The path was `lj_gc_pubroot -> lj_gc2_barrier_tv_g -> retained table -> early
direct-body preservation`. The direct-body probe is tactical: the retained
table identity is still published for semantic traversal, and an ordinary
table operation separately publishes its table-generation read epoch before
loading the generation it will dereference. A collision with the opportunistic
retired-body writer therefore must skip this early optimization, not convert a
valid `SWEEP_OPEN` cycle into permanent `NO_RECLAIM`.

`gc2_preserve_tab_direct_bodies()` now:

1. tries one outer SMR read without waiting;
2. validates the current array and node through the existing
   `lj_tab_*_snapshot_gc_held()` APIs while the table allocation scope is held;
3. marks only those validated current raw generations (nested SMR admission is
   reentrant under the outer reader);
4. releases the outer reader; or, if admission loses, simply leaves the already
   retained/published table for normal semantic traversal.

This is the same proof already used by `threading_root_table()`: early vector
preservation is optional when tactical SMR admission loses, while the table
identity and the table-read epoch remain the semantic and dereference lifetime
authorities. No raw array/node header is read before the successful outer SMR
admission and validated snapshot.

## Before/after counters

The original `m4_threading_api` hang reached a representative snapshot of:

- `major_root_scans = 319709`;
- `thread_scan_needscan = 319461`;
- `thread_scan_owner_needscans = 699`;
- `hs_pending = 0`, with SSB publication/drain nearly balanced.

With the composed fix, the 24-round native worker reproducer reported 19 major
root scans after its first reply and 160 after the twenty-fourth. Each round
contains two explicit steps and one full collection; the steady increase is six
scans per round, with an occasional seventh boundary scan, rather than an
unbounded same-cycle rescan.

## Validation

- The previously hanging complete `t-threading-api.lua -joff` fixture completed
  in about seven seconds in a symbolized build.
- The exact reproducer advanced through both dropped-live explicit collections,
  released the rendezvous worker, completed two post-exit collections, and
  finished every remaining API/join/mutex assertion.
- A release-like `-Werror` build completed.
- The 24-round native secondary-collector reproducer completed ten consecutive
  runs. The complete active-root Lua fixture then passed in both `-joff` and JIT
  modes in the suite, followed by three additional consecutive pairs against
  the final restored build.
- `m3_gc_active_thread_roots` passed its C explicit-assist fixture plus both Lua
  JIT modes.
- `m4_threading_api`, `m4_thread_gcprep`, and `m4_threading_live_root` passed.
- `m3_gc2_recovery` passed its normal and assertions-plus-GC2-paranoia C
  variants.
- `m3_gc_root_pending_race` passed its load/exchange/CAS activation-race
  regression.

This is an x86-64 Linux focused handoff for the beta GC2/JIT run gate. It is
not a claim that the wider b1.2.1 nonblocking inventory or other targets are
complete.
