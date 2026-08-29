# Pre-grace typed semantic destruction

## Status and scope

This tranche lets the main-TG SWEEP owner perform semantic destruction of an
unmarked, rootless, typed small-arena allocation before the arena-wide physical
reuse grace. It is deliberately restricted to the three fixed-layout kinds
whose immutable arena identity is already published:

- `LFUNC0`,
- `LFUNC1`, and
- `CLOSED_UV`.

The semantic destructor releases the closure or closed-upvalue resources and
subtracts its GC charge immediately. The allocation boundary, READY bit,
destructor kind, and body bytes remain physically present until the normal
arena quarantine grace completes. No cell is reused early.

This removes the need to recognize a special closure/upvalue pair. Every start
is admitted, validated, destroyed, or retained independently from its immutable
kind and exact extent. In particular, an adjacent closed-upvalue disagreement
does not veto destruction of an independently valid `LFUNC1`, and no C-call or
FFI-shape matcher is involved.

## Why the ordinary retirement path was not enough

The earlier sidecar path classified each dead typed start as `WHITE ->
RETIRED`, incremented `reclaim_deferred`, requested a full arena grace, and ran
the semantic destructor only after grace. That is the general safe fallback,
but closure-heavy active-GC workloads repeatedly pay its per-object retirement,
deferred-accounting, and post-grace finish costs.

A typed object may be destroyed earlier only when the collector can prove both
that no semantic mark producer was omitted and that its body cannot be mutated
or reclaimed while it is inspected. Merely observing a global exclusivity bit,
an empty queue, or a sealed arena is not such a capability. The new path
therefore combines a locally acquired global gate, an SMR read lease, an exact
arena seal, current-cycle root/fixpoint evidence, and a per-object lifetime
claim. Any failed predicate falls back to the existing retirement path.

## Exclusive admission transaction

`lj_gc_sweep_gc2_arena_unmarked()` keeps its original two-argument interface
and non-exclusive behavior. Main-TG sweep-owner progress calls the new internal
`lj_gc_sweep_gc2_arena_unmarked_exclusive()` entry point. Secondary TGs continue
to use the original path.

The exclusive wrapper attempts the following capability:

1. locally tries to acquire `mt_gc_exclusive`;
2. after that succeeds, tries to enter one outer SMR reader;
3. flushes pending roots and repairs the ownership spine;
4. verifies the SWEEP phase, bridge/root snapshot, activation state, main-TG
   identity, internal arena allocator, zero mutator entry/live/active counts,
   configured/active worker state, closed JIT gate, idle recorder, empty
   recovery/assist/weak/SSB/grey/thread/table/finalizer work, and the exact arena
   owner/flags;
5. locally seals the target arena;
6. clears only the completed snapshot generation's count-zero `PENDING` bit;
7. rechecks the complete certificate and exact remote state `CLOSED|SEALED`;
8. holds that seal across the bounded arena scan; and
9. unseals with `keep_pending=1`, leaves SMR, and releases the global gate on
   every exit.

Losing either global-gate or SMR admission disables only early body access. It
still runs the original sidecar-only classifier before owner progress moves the
arena from NEEDSWEEP to quarantine. In particular, an SMR conflict cannot skip
classification and accidentally quarantine a still-WHITE dead start.

The completed SWEEP root snapshot naturally leaves a conservative
`CLOSED|PENDING` arena state. Clearing it is sound only inside the complete
certificate above. A new publisher first installs its count/`PENDING` intent;
it therefore defeats either the clear/recheck or a later exact per-object
commit check. The path never waits for a producer and never treats a pre-held
`mt_gc_exclusive` value as locally owned authority.

## Claim, validate, commit

No target header or body byte is read while its lifetime is merely `LIVE`.
Admission for one supported start is:

1. derive the exact extent from the immutable sidecar kind;
2. check only global state and arena side planes, expecting `LIVE`, `WHITE`, an
   unmarked exact start, READY, root `NONE`, recovery `IDLE`, no late bit, and
   exact `CLOSED|SEALED` remote state;
3. atomically claim `LIVE -> DESTRUCT`;
4. execute the sequentially consistent fence paired with rescue admission;
5. repeat the complete global, arena, and exact-start predicate, now expecting
   `DESTRUCT`;
6. validate the function/upvalue header and exact layout under that body lease;
7. execute the final fence and repeat the complete predicate;
8. atomically commit `DESTRUCT -> FREE`, then `WHITE -> FREEING`; and
9. dispatch directly from the immutable kind to `lj_func_free()` or
   `lj_func_freeuv()` exactly once.

Recovery may cancel the tentative claim with `DESTRUCT -> RESCUE`. A lost claim
or commit accepts `RESCUE` and a recovery-restored `LIVE` as ordinary outcomes;
recovery owns the durable work publication and restoration. Rollback never
steals that ownership. Unexpected lifetime loss or failure of the exact WHITE
commit is a release-build fail-stop, not an assertion-only continuation.

The SC lifetime handshake prevents both a body reader and a racing rescue
publisher from missing one another. The arena seal alone is not used as a body
lease. Once `DESTRUCT -> FREE` succeeds, no competing semantic destructor can
acquire the object, and the direct typed call performs accounting once. An
observed exact `FREE` plus `FREEING` outcome is terminal ownership, not a reason
to dispatch again.

## Accounting and physical completion

Pre-grace dispatch calls the normal semantic destructor with the lifetime
already committed to `FREE`. Deferred arena free therefore:

- subtracts the exact `sizeLfunc(0)`, `sizeLfunc(1)`, or `sizeof(GCupval)`
  charge once;
- preserves the body, allocation boundary, READY bit, and destructor identity;
  and
- creates no `RETIRED`/`reclaim_deferred` ticket.

The sweep still requests an arena grace. Post-grace quarantine recognizes the
already-owned `FREE|FREEING` start, removes its physical discovery state and
kind, and does not charge or dispatch it again.

If any global, arena, sidecar, body, cdata, permanent-retention, or lifetime
predicate disagrees, the unchanged fail-closed path classifies the start as
`RETIRED`, increments the deferred ticket, and validates/destructs it after
grace. A concurrent semantic mark rescues `RETIRED -> LIVE` with the existing
accounting repair. Unsupported kinds are never inspected pre-grace.

## Compatibility boundary

There is no public LuaJIT API or ABI change and no new bytecode. The added
exclusive entry point is internal. The original arena-scan function retains
its historical zero return contract.

The separately documented temporary custom-`lua_Alloc` policy is unchanged:
`lua_newstate()` currently uses the internal arena allocator and
`lua_setallocf()` remains a no-op. Restoring arbitrary allocator callbacks is
still required after the beta boundary described in
`lua-alloc-temporarily-disabled-2026-07-10.md`; this optimization neither
claims nor weakens that future requirement.

## Validation

Focused coverage constructs real rootless `LFUNC0`, `LFUNC1`, and adjacent
closed-upvalue allocations, then verifies:

- exact pre-grace semantic accounting and `FREE|FREEING` ownership;
- unchanged body bytes, block boundaries, READY bits, kinds, and root `NONE`
  before grace;
- removal of physical discovery metadata after grace with no second charge;
- independent closure destruction when the adjacent upvalue has an injected
  cdata disagreement, followed by only that upvalue's post-grace destruction;
  and
- ordinary retirement when the global exclusive bit was pre-held instead of
  locally acquired or when the locally acquired global gate then lost SMR
  admission.

After the claim-before-body implementation, the forced-clean focused fixture
passed once and that exact frozen binary passed 20/20. After the later
release-build fail-stop and SMR-loss fallback hardening, a clean default rebuild
and the expanded forced-clean focused fixture passed in 22.98 seconds. The
following post-claim matrix also passed first try:

| Target | Result | Time |
| --- | --- | ---: |
| `m2_arena_sweep` | PASS | 1.47 s |
| `m6_jit_fnew_bump` | PASS | 22.94 s |
| `m3_gc2_recovery` | PASS (normal, paranoia, default restore) | 131.82 s |
| `m3_gc2_paranoia` | PASS (all C fixtures, stock 509/509, no-JIT 387/387, restore) | 93.08 s |
| `m6_jit_gc2_readiness` | PASS | 1.29 s |
| `m3_gc2_worker_scheduler` | PASS | 42.72 s |

The known nonfatal GCC `gc2_root_rescan_later`/`la_load32_acq`
`-Wstringop-overflow` diagnostic remains unchanged.

## Performance and follow-up

Five independent `BENCH_SCALE=.1 closures_upval` processes were measured for
each interpreter/mode after the final default rebuild. Each process retains the
benchmark's own best-of-five inner timing:

| Mode | Current ns/op (five runs; median) | Stock `/usr/bin/luajit` ns/op (five runs; median) | Ratio |
| --- | --- | --- | ---: |
| GC active | 487.01, 487.09, 489.12, 494.25, 488.80; **488.80** | 37.91, 37.60, 37.63, 37.45, 37.34; **37.60** | **13.00x** |
| GC stopped | 102.83, 113.84, 105.72, 101.42, 94.52; **102.83** | 18.43, 20.68, 20.84, 20.93, 21.11; **20.84** | **4.93x** |

The stopped measurements require an explicit wrapper. `BENCH_GC_MODE=stop`
is ignored by the current benchmark (it recognizes only `generational` and
`incremental`), while a simple `-e 'collectgarbage("stop")'` is undone by the
benchmark's next explicit `collect`. The valid wrapper runs each real
`collect`, immediately stops automatic GC again, and was probed with
`collectgarbage("isrunning") == false` before sampling:

```sh
-e 'local real_collectgarbage=collectgarbage; collectgarbage=function(op,arg) local r=real_collectgarbage(op,arg); if op=="collect" then real_collectgarbage("stop") end; return r end; real_collectgarbage("stop")'
```

The current active-minus-stopped delta is 385.97 ns/op; stock's is 16.76
ns/op. The active median is also 12.72% slower than the preceding rootless-kind
checkpoint's valid 433.63 ns/op active measurement while stock remained stable.
The repeated full predicate/claim work has therefore not yet amortized the
retirement it removes. This tranche is a correctness and ownership prerequisite,
not a claim that the b1.2.0 performance gate has passed; both base closure
allocation and active collector work remain release blockers.

The next likely closure-churn work is to reduce post-grace per-start scanning
and then restore active-MARK inline FNEW only under an explicit current-cycle
proto/environment/upvalue traversal certificate. Relaxing the mark lease or
blindly raising collector thresholds would trade away the safety established
here and is not an acceptable optimization.
