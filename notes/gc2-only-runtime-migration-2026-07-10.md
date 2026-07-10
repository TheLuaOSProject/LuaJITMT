# GC2-only runtime migration audit, 2026-07-10

The target invariant is now stricter than a dual-collector bridge: GC2 must be
the only collector that executes, including shutdown, while the public
`lua_gc` API and allocator ABI retain their LuaJIT behavior. The plan files are
unchanged. This note records the required divergence and, importantly, does not
claim that the current checkpoint has completed it.

## What is already GC2-only

Automatic allocation checks, interpreter/JIT GC steps, `LUA_GCCOLLECT`, and
`LUA_GCSTEP` all enter GC2. Production code no longer advances the old
`GCSstart`/`GCSpropagate`/`GCSatomic`/`GCSsweepstring`/`GCSsweep` state machine.
The old-looking `gc.total`, `gc.threshold`, pause, and step-multiplier fields are
generic accounting/pacing compatibility fields and may remain after the color
collector is deleted.

## Legacy collector code which still executes

1. Runnable inbound-trace rescue calls `lj_gc_markobj_deep()`, which enters the
   old `gc_mark()`/`gc_propagate_gray()`/`propagatemark()` traversal engine.
   This is unsafe even as a temporary bridge: it can blacken objects while the
   compatibility state says `GCSpause`, re-enabling color barriers whose grey
   work no production collector drains.
2. Trace retirement still invokes both color and GC2 preservation branches.
   The `gc2 == 0` branch clears old white bits before GC2 performs the real
   preservation.
3. `threading_result_gc_handoff()` calls `lj_gc_markobj()` before its GC2
   preserve, and finalizer requeue still calls `makewhite()`.
4. `lua_close()` and failed-state construction call `lj_gc_freeall()`, whose
   authoritative destructor walk is the old `gc_fullsweep()`/`gc_sweep()` plus
   `gc_sweepstr()`. Arena cleanup afterward does not make shutdown GC2-only.
5. `GCSfinalize` remains a live compatibility latch for spawned finalizers.
   It must become an explicit GC2-owned flag rather than keeping `gc.state`
   semantically active.

These paths are migration blockers, not acceptable permanent compatibility
shims.

## P0 prerequisites for deleting the old shutdown sweep

### Interned strings

GC2 currently has no runtime string-table reclamation domain. The old
`gc_sweepstr()` runs only at shutdown, while `lj_str_sweep_claim()` has no
production caller and would wait for active interners. GC2 needs lock-free
bucket unlink/tombstone publication followed by SMR-delayed string-body free;
unreachable interned strings otherwise remain until close.

### Public custom allocators and `lua_setallocf`

GC2 arena bitmaps cannot mark or sweep objects created by an arbitrary public
`lua_Alloc`. Classification currently accepts such objects but supplies no
mark epoch or delayed-free registry. Changing the allocator can also make
pre-existing arena bodies appear custom and route later collection through the
wrong lifetime domain. This was reproduced by the extended
`t-ffi-ccall-error-state.c` allocator-clobber stress: the errno/OOM carrier test
passes, but keeping the wrapper installed across a new GC2 cycle corrupts live
global reachability. The extended matrix is therefore opt-in under
`LJ_FFI_ERRSTATE_ALLOC_STRESS` until this ABI blocker is fixed.

The required replacement is an exact GC2 custom-allocation registry with mark
epochs and grace-delayed destructors, plus an atomic allocator-configuration
generation covering `lua_setallocf()`. The normal `lua_newstate(custom_alloc)`
and `LUAJIT_USE_SYSMALLOC` cases must use it too.

### Exact arena object identity

Active-black JIT bump allocation can mark tables, functions, and upvalues as
arena-owned without publishing them to the ownership spine. The corresponding
`*_arenaowned` predicates currently have no consumers. A later sweep cannot
distinguish those bodies from raw storage, so the conservative arena scan marks
them live indefinitely.

The immediate correctness fallback is to publish every such object through the
per-TG pending-root chain. The scalable design is per-arena exact object-start,
type, extent, and variable-cdata header-offset metadata. That metadata lets GC2
enumerate destructors without retaining `gc.root` as a global ownership spine
and closes the same ambiguity for shutdown.

## Migration order

1. Replace trace/thread/finalizer color mutations with phase-aware GC2
   preserve/mark/sweep-root operations; add regressions proving no old marker
   is called.
2. Stop omitting arena-owned objects from exact GC2 discovery, then land the
   per-arena identity sidecar.
3. Add nonblocking string-table sweep and the custom-allocation registry.
4. Implement GC2 close/failure drain, including finalizers, strings, custom
   objects, traces/mcode, and all arena side bodies.
5. Delete the old marker, grey lists, weak bridge, sweep functions, color
   barriers, `gc.state`, `currentwhite`, `gc.sweep`, and `gc.sweepstr`; replace
   `GCSfinalize` and the close test with explicit GC2 flags.
6. Convert bridge-oriented tests to assert that legacy marker/sweeper entry
   counters remain zero in normal execution and shutdown.

Until those steps land, this fork is GC2-driven during normal collection but is
not yet honestly describable as “100% GC2-only.”
