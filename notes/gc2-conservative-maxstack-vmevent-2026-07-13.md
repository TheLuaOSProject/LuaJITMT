# Conservative GC2 maxstack validation after VM events (2026-07-13)

This checkpoint fixes the pre-existing recorder-resume failure exposed after
the bounded JIT trace-pressure change. The production change is intentionally
small: both GC2 stack scanners now classify every remote/native/JIT-owned
`maxstack` snapshot as conservative. The plan files are unchanged.

## Failure and exact stale edge

`m6_jit_token` races `jit.attach()` removal and collection against
`lj_vmevent_prepare()` on a fresh coroutine whose stack must grow. A successful
prepare publishes the TRACE handler on the coroutine stack, the callback runs,
and the C fixture restores the original `L->top`. The old handler TValue then
remains as ordinary popped stack storage above the active top.

The observed path was:

1. `setfuncV()` publishes the handler for `lj_vmevent_prepare(TRACE)`;
2. trace callback work passes through `trace_start()` / `trace_hotside()`;
3. the fixture pops the callback window and a peer removes the registry root;
4. a later SWEEP scans the coroutine while it is remote/native/JIT-owned; and
5. the scan reaches the old handler five TValues above the current top.

The remote/native/JIT branch in both `gc2_stack_scan_top()` and
`gc2_stack_scan_top_worker()` deliberately returns `L->maxstack`: its owner has
not published a precise stable boundary. Unlike the existing invalid-frame and
scan-limit `return max` paths, that branch left `conservative` false. Its caller
therefore treated every raw tail cell as a valid semantic TValue without first
checking allocator membership, object lifetime, or tag/header agreement.

Once the handler allocation was reclaimed or reused, the function-shaped stale
word entered semantic SWEEP marking and ultimately caused
`gc2_trace_sweep_edge()` to publish sticky `recovery_failed`. The later
"recording should resume after token release" assertion was only a downstream
symptom: SWEEP recording is correctly rejected after a real recovery failure.

## Repair and lifetime argument

Both widened branches now set `*conservativep = 1` before returning
`maxstack`. Their only consumers already load each TValue atomically and call
`gc2_tv_gcref_type_match()` whenever that flag is set. The validator:

- rejects stale or malformed collectable tags before semantic dispatch;
- proves arena/huge allocation membership and header/type agreement inside a
  scoped observation admission; and
- lets the later MARK/SWEEP semantic marker acquire its own exact lifetime
  admission, so releasing the validator scope does not expose a use-after-free.

Address reuse between the two admissions can conservatively retain the
replacement allocation, but cannot make the old allocation readable again.
Frame functions remain separately validated and marked before the raw payload
scan. Same-thread VM collections keep their precise active-top path, so popped
values do not become ordinary weak/FINREG roots.

Remote/JIT scans remain non-authoritative and request an owner `NEEDSCAN`
handoff. Native-parked stacks may publish an authoritative stamp because the
owner is not mutating the stack. A full source audit found no third GC2 loop
which walks stack payload through `maxstack`.

## Discarded false fixes

Several diagnostic drafts were fully removed before this checkpoint:

- anchoring the handler elsewhere or clearing its popped stack cell hid this
  particular stale word but did not make arbitrary popped/spill cells safe;
- changing VM-event stack order was unnecessary once the widened snapshot was
  validated; and
- retrying recorder entry after a GC rejection treated the downstream symptom,
  could restart stopped collection, and did not repair the unsafe root read.

One discarded retry draft also stored attempt state in shared `J->gc_retry`.
The recorder token is released before the caller consumes that field, so a peer
attempt could overwrite it. Any future attempt-local status must remain in the
caller's stack-owned context.

## Strengthened oracle

`t-jit-token.c` now runs 48 prepare/detach rounds and alternates two schedules:

- prepare/call/pop before allowing handler detach and collection, which
  deterministically creates the stale raw tail; and
- detach first, preserving the original race/drop schedule.

The C helper reports both stack growth and successful handler preparation. The
Lua fixture requires at least one successful stack publication, and C asserts
that `recovery_failed` is clear before and after the race.

Sticky multithreading deliberately keeps retired trace bodies in their public
slots through SMR grace. Fixed searches of trace IDs 1 through 32 therefore
reported false failures once the low namespace was reserved. All later
busy/resume/live-trace counters and the targeted-flush selector now scan 1
through 1024, covering the default `maxtrace=1000` namespace.

The same pressure can leave `phase=IDLE` while a pending `cycle_leader` owns an
IDLE-to-MARK request and has closed the JIT phase gate. The IDLE-reclaimer
fixture previously asserted one-shot admission in that valid window. Its
test-only settle helper now consumes a pending request, drains the resulting
cycle, and requires `IDLE`, no leader, and an open gate before exercising the
separate reclaimer transaction. No production wait or retry was added.

## Remaining validator debt

`gc2_tv_gcref_type_match()` is Boolean. A stale word and a transient validation
retry are both returned as false: small-arena admission failure and huge-reader
overflow propagate as retry from `gc2_observed_obj_status_scoped()`, then
`gc2_observed_obj_valid_scoped()` collapses every non-positive status.
Conservative stack callers skip that cell and may later stamp the scan complete.

This behavior predates the fix in invalid-frame/scan-limit fallback paths, but
the corrected flag makes it apply to every widened remote/native/JIT snapshot.
The proper follow-up is a stack-specific tri-state validator:

- invalid stale cells are skipped;
- an owner scan which sees retry does not publish a completed scan stamp; and
- a worker scan which sees retry requeues the thread.

Blindly treating all invalid words as retry would livelock on normal popped
storage and undo this repair. Combining validation and semantic mark admission
would also remove the current duplicate admission for valid GC cells.

The temporary custom-`lua_Alloc` omission remains explicit. When arena-backed
allocation is disabled, the compatibility branch still trusts the candidate
header directly; this checkpoint does not claim safe arbitrary custom-allocator
stack validation. That limitation remains temporary as previously requested.

## Performance

There is no VM dispatch, recorder, trace-entry, allocation, or same-thread GC
hot-path change. Cost is confined to GC scans already forced to walk a remote,
native, or JIT-owned stack to `maxstack`. Non-GC cells take the validator's tag
fast path. Valid collectable cells currently pay observation plus semantic
admission, so dense remote stack snapshots can make a collection somewhat more
expensive; the tri-state combined-admission follow-up can recover that cost.

## Validation

The final optimized fixture passed:

- one focused `m6_jit_token` suite run, including secondary-TG recording and
  explicit x64 exits;
- 200 independent optimized fixture processes at four-way concurrency;
- `m3_gc2_recovery` in normal and paranoia configurations;
- `m6_jit_gc2_readiness` (hard cadence plus MARK/SWEEP cooperative JIT);
- `m6_jit_vmevent_flush`; and
- `m6_jit_flush_thread_stress`.

`m3_gc_active_thread_roots` still fails its synthetic grey-drain count assertion
at `t-gc-active-collect-assist.c:174`. It failed twice on this tree and once on
an independently rebuilt clean `071b345a` parent at the identical assertion,
so it is pre-existing fixture/threshold debt and not attributed to this change.
The actual active-stack, recovery, VM-event, and JIT readiness coverage above is
green.
