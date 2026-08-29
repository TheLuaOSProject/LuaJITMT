# b1.2 explicit sole-mutator string reclamation (2026-07-12)

## Release boundary

b1.2 does **not** enable the incomplete concurrent canonical-string close
protocol. `LJ_GC2_STRING_BODY_RECLAIM` remains zero. Physical string-body
reclamation is enabled only as an opportunistic part of an explicit
`lua_gc(L, LUA_GCCOLLECT, 0)`/`collectgarbage("collect")` operation.

This narrow path removes the severe history-dependent memory and string-table
performance cliff without claiming that Stage B--F quarantine commit is done.
Fully concurrent reclamation, native borrow epochs, QCOMMIT/E2 arbitration,
post-commit substitution, and address-reuse quarantine remain b1.2.1 work.

The temporary internal-allocator-only policy also applies here. A custom
`lua_Alloc` request simply fails admission and retains string bodies.

## Exact admission

The string subsystem owns two one-shot words:

- `reclaim_requested`, set only around the public explicit full-collection API;
- `reclaim_exclusive`, claimed with a nonwaiting zero-to-one CAS at the
  major-cycle string-sweep boundary.

Admission requires all of the following:

- the internal arena allocator and an internal main TG;
- the executing TG is exactly `g->main_tg`;
- no live or entering secondary mutator;
- no configured GC worker pool;
- no queued finalizer phase work;
- no active JIT TG;
- no Stage-A quarantine identity (`qcount == 0`);
- no old/current sole-mutator batch.

Failure skips string reclamation for that collection. It never waits for a
mutator, worker, recorder, finalizer, resize owner, or quarantine owner.

First attach/spawn publication increments `mt_entering`, takes an SC fence, and
then checks `reclaim_exclusive`. The collector publishes its exclusive CAS,
takes the matching SC fence, and rechecks `mt_entering` and `mt_live`. The
entrant also rechecks after publishing `mt_live`. Therefore the store-buffering
outcome in which both sides believe they won is forbidden. A losing entrant
retains its entering reservation and yields until the short collection gate is
released.

GC-worker enable performs the analogous transaction. It checks the gate,
release-publishes `n_workers`, fences, and rechecks before creating any OS
worker. A losing enable rolls the advertised pool back while still holding the
worker-control token. The collector never stops or joins an existing pool; a
nonzero pool simply defeats admission.

`lua_gc` runs the collector in a protected C call. The one-shot request is
cancelled outside that protection before an error is rethrown. Post-admission
operations are non-throwing: batch allocation is the arena nothrow primitive,
queued finalizers and active JIT execution are excluded, and ownership failures
fail closed rather than reopening canonical admission.

## Batched ownership and allocator handoff

One `StrRetireBatch` owns up to 256 string-body pointers. This replaces the old
prototype's allocation of one roughly 80-byte `StrCanonRec` per dead string.
Metadata cost is one pointer per string plus one header/allocation per 256
strings.

For each exact main-table unlink:

1. an initialized current batch is release-published in `g->str.sweep_batch`;
2. the body pointer and inclusive count are release-published;
3. `sweep_batch_pending` is published;
4. the incoming main-table edge is CAS-unlinked;
5. a losing CAS rolls the provisional slot back; a winning CAS clears pending
   and leaves the inclusive slot as the durable body owner.

The batch allocation is globally discoverable before its GC2 accounting
checkpoint. `lj_gc_total_add(sizeof(batch))` and
`lj_gc2_account_alloc(sizeof(batch))` exactly match the one eventual
`lj_mem_free(sizeof(batch))`, in normal IDLE drain or joined-world terminal
cleanup.

Full batches and the final partial batch are sealed onto a Treiber list. String
TAG grace and post-UNLINK grace still run. The batches are not physically
destroyed while GC2 SWEEP is active: traversable arenas correctly retain
`CLOSED|PENDING` mutation state during that phase. `lj_str_gc2_sweep_finish`
keeps exclusion and the sealed ownership list through SWEEP-to-IDLE. The final
IDLE handshake enters the existing exact-thread SMR reclaimer and drains every
batch through the reclaim-held destructor path. Only an empty batch/current
set permits `reclaim_exclusive` to reopen.

The generic arena object destructor already refuses `LJ_TSTR`; the intern-table
owner remains the only component which mutates `gct`, `g->str.num`, and the
body allocation. Main-TG string-count credits are flushed at admission, so
every later body destructor subtracts from an exact global count.

Abort before an unlink releases the empty gate normally. Crossing an unlink
handoff or attempting to abort after irreversible batch retirement is fatal:
releasing the gate in either state could permit a duplicate canonical identity.
Joined-world shutdown consumes a complete current/sealed batch, clears an
unconsumed request, and releases the gate only after terminal body cleanup.

## Measured effect

Pinned `cd854a9a` baseline and this candidate, Linux x86-64 release builds,
five fresh processes, 20,000 unique strings per process:

| build | allocation median | explicit GC median | retained after GC |
|---|---:|---:|---:|
| gated-off baseline | 311.25 ms | 0.878 ms | 1755.3 KiB |
| 256-body batch candidate | 302.67 ms | 6.817 ms | 518.3 KiB |

The reclaiming collection adds about 0.30 microseconds per created string, but
allocation plus collection is slightly faster in this workload and retention
drops by about 1.21 MiB. This is intentionally a correctness/reclamation target,
not the final b1.2.1 concurrent-string performance target.

An eight-round, 10,000-unique-string Lua churn retained about 262 KiB after the
candidate's explicit collections instead of growing with every round. The
remaining footprint includes the enlarged intern-table generation and arena
granularity, not unreclaimed string count: the internal C regression checks the
exact string-count drop and equality of unlinked/reclaimed counters.

## Regression gates

- `m5_strtab_cas`, including the new `t-str-reclaim-sole.c` fixture:
  - 10,000-body exact string-count and memory shrink;
  - unlinked/reclaimed batch-counter equality and empty ownership state;
  - deterministic attach publication after the exclusive CAS, proving the
    collector backs out and the entrant attaches/detaches;
  - a later uncontended explicit collection reclaims the retained strings;
  - terminal cleanup with a requested but unconsumed pass.
- existing canonical-quarantine and string-table CAS fixtures;
- four-thread string interning/GC stress;
- GC worker enable/control regression;
- finalizer-spawned worker regression;
- GC2 paranoia and stock JIT/no-JIT suites;
- Clang AddressSanitizer build plus the exact reclaim fixture and Lua churn.

The source diff applies cleanly to b1.2 checkpoint `a73ceb97`. The implementation
note, tests, and source changes are composition-friendly and do not edit the
normative files under `plan/`.
