# Temporary internal-arena-only `lua_Alloc` policy

Date: 2026-07-10

Status: active temporary divergence. This does not change `plan/`; it narrows
the currently enabled allocator surface while GC2, the VM, JIT, and FFI are
made fully lockless. The public function signatures and exported ABI remain in
place, but arbitrary allocator callbacks are not currently executed.

## Effective behavior

- `lua_newstate(f, ud)` accepts the stock arguments but creates the state with
  LuaJIT-MT's internal arena allocator and does not call `f`.
- `lua_setallocf(L, f, ud)` is a no-op. It cannot switch the ownership domain
  of existing or future allocations.
- `lua_getallocf(L, &ud)` reports the allocator actually used by the state: the
  internal arena callback and its internal context.
- `luaL_newstate()` and a build carrying `LUAJIT_USE_SYSMALLOC` also remain on
  the internal arena while this gate is enabled.
- The policy is controlled by the internal
  `LJ_GC2_INTERNAL_ALLOCATOR_ONLY` gate. It defaults to one. Setting it to zero
  exposes the unfinished legacy custom-allocator paths and is not a supported
  configuration yet.

This is not presented as completed Lua API behavioral compatibility. It is a
temporary safety boundary explicitly accepted while the core lockless runtime
is completed. It does not re-enable or retain the old collector: GC2 remains
the only runtime collector.

## Why the partially implemented registry was not enabled

An exact allocation registry is necessary but not sufficient. Review of the
first registry prototype found several independent ways a nominally exact
record could still permit memory corruption:

1. A stable metadata record did not pin its allocation body. A concurrent
   malloc-style stack realloc/free could invalidate storage already loaded by a
   GC2 remote stack scanner.
2. `MOVING` and `FREEING` records became invisible to lookup. Classification
   could then fall through and mistake a wrapper-owned arena address for an
   ordinary live arena allocation while its callback was changing it.
3. A callback which delegates to `lj_arena_allocf` needs two coordinated mark
   identities: callback ownership in the registry and physical liveness in the
   arena bitmap. Advancing only the registry epoch lets arena sweep reuse a
   still-live body.
4. Early-linked prototypes could be observed while their record was
   `PENDING_GCO`; stale allocation bytes could look like a valid type, while an
   invalid pending root could truncate a detached pending-root chain.
5. RAW/PENDING records require exact kind, object alias, expected type, and size
   checks at every traversal gate. Merely finding a registered base is not
   permission to read a GC header.
6. Concurrent publication of an interior object alias needs a single
   linearization point. Duplicate insertion of one intrusive record can create
   a self-cycle.
7. The recorded size, not a racy destructor field or caller-supplied `osize`,
   must be authoritative for callback dispatch and accounting.
8. Allocator metadata OOM, same-address realloc, failed realloc, and terminal
   teardown all need bounded record recycling and exact rollback. A terminal
   drain cannot stand in for runtime traversal/destructor correctness.
9. Stock-compatible allocator callbacks and their `ud` values may assume
   serialized calls or may be temporary wrappers. Retaining old callback pairs
   and invoking them concurrently changes those lifetime and concurrency
   assumptions. An arbitrary callback can itself block, so the runtime cannot
   manufacture a wait-free guarantee around it.

Leaving these paths enabled merely because basic allocation-balance tests pass
would hide correctness failures behind `lua_close()`'s final physical drain.
The internal arena already has the bitmap, HugeTab, quarantine, and handshake
identity needed by GC2, so it is the sound enabled ownership domain now.

## Re-enable gates

Arbitrary `lua_Alloc` support can be restored only after all of the following
are implemented and tested together:

1. An exact base/object registry with immutable allocator-generation ownership,
   expected type, exact extent, backing kind, and collision-safe publication.
2. Tri-state classification which treats LIVE as usable, RETIRED as rescuable,
   and MOVING/FREEING as a tombstone that forbids every arena fallback.
3. GC2-SMR retirement of external bodies. Realloc must publish the replacement
   while keeping the old body readable until remote stack/object readers have
   crossed a grace period.
4. Dual registry/arena marking and one destructor claim for allocator wrappers
   which return arena-backed pointers.
5. Initialize-then-publish constructors, including parser/bytecode prototypes,
   variable cdata, strings, open upvalues, traces, and every intrusive root-list
   insertion path.
6. A documented callback execution contract. The implementation must preserve
   stock single-threaded allocator behavior where required and clearly isolate
   any inherently blocking user callback from the nonblocking internal arena
   fast path.
7. Bounded metadata allocation/recycling, injected metadata-OOM rollback, and
   runtime reclamation/destructor counters which cannot be satisfied solely by
   terminal shutdown.
8. Deterministic stress for allocator A -> B -> A transitions, concurrent
   setters and secondary TGs, wrapper-backed arenas, sysmalloc, invalid pointer
   rejection, active-MARK proto construction, stack relocation, JIT traces,
   FFI fixed/VLA cdata, weak tables, finalizers, and shutdown.
9. Linux, Wine/Windows, and Darling/macOS validation plus APICHECK, sanitizers,
   and performance comparison showing zero cost on the normal internal path.

Until those gates are met, work should optimize and complete the internal arena
and GC2 paths without adding conditionals for a custom ownership mode. When the
feature returns, removing the named policy gate must be a deliberate tested
milestone, not a silent side effect of another GC change.
