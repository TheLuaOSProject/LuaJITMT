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

## First GC2-only trace slice

Runnable inbound-trace rescue and every retired-trace/vector preservation path
in `lj_trace.c` now execute GC2 operations exclusively. A persistent runnable
machine-code edge rescues its target with `lj_gc2_trace_sweep_root()`, which
publishes sweep-time traversal through the current TG's SSB and falls back to
an ordinary semantic GC2 mark outside sweep. Retired trace bodies, their KGC
operands, prototype/PC owners, exit tables, and retired trace vectors use GC2
body or raw-memory marks only. No retirement path calls
`lj_gc_markobj_deep()`, `lj_gc_preserveobj()`, or the color-bridge arena-mark
wrappers, and the compatibility `gc2` argument to `lj_trace_markvecs()` no
longer selects a collector.

The recorder's pre-publication start-prototype guard is GC2-only as well. It
uses the same phase-aware sweep-root operation whenever GC2 is active and the
marked-prototype barrier closes the active-black/rescan case. Recording in GC2
IDLE keeps its previous fast path: the prototype is already a stack semantic
root, so no extra bitmap operation is needed merely to compile it.

The focused retirement fixture gives the synthetic target and unpublished
retired body real legacy white bits, then verifies that inbound rescue and
retire-list publication leave those bits unchanged while the target is present
in GC2's mark domain. This would fail under the removed color paths.

The pre/post retire-list publication marks remain deliberate: they close the
race with an already-running arena sweep and with a GC2 root scan starting
between preservation and intrusive-list publication. Removing the duplicate
collector does not remove either publication side.

## Second GC2-only runtime and shutdown slice

The live legacy entry counter now covers `lj_gc_markobj()`,
`lj_gc_markobj_deep()`, `gc_mark()`, `gc_propagate_gray()`, per-object
`propagatemark()`, `gc_sweep()`, and `gc_sweepstr()`, split between runtime and
shutdown. A JIT/interpreter/table/closure/weak-graph workload, repeated full
GC2 cycles, `jit.flush()`, and `lua_close()` all require every counter to remain
zero.

That assertion is now structural as well as observational:

- native-root publication, thread-result handoff, finalizer discovery/requeue,
  FNEW operand protection, upvalue close/store, table/object barriers, and trace
  publication no longer call or mutate the legacy marker/frontier;
- active-black table/function/upvalue bump objects always publish their exact
  headers through the per-TG pending ownership chain, including the x64 TNEW
  and traced FNEW inline paths; the dead arena-owned flag scheme was removed;
- `lua_close()` and failed-state teardown call `lj_gc2_freeall()`. Its terminal
  single-threaded drain first stops and joins parked GC2 workers, disconnects
  traces while their prototypes/bytecode are alive, reanchors registered and
  userdata-owned child states without pre-freeing them, and clears
  `vmthread_ref` before a custom allocator can release that state. It then walks
  the repaired GC2 ownership spine, runs exact destructors, drains roots
  published by closing upvalues, deduplicates corrupt intrusive cycles, drains
  the current intern table without color decisions, and retains only the
  super-fixed main state. Seen-set OOM stops conservatively instead of touching
  an untracked tail. The old `gc_sweep()` and `gc_sweepstr()` implementations
  have been deleted;
- finalizer-spawn callback scope and outliving-worker deferral now use a
  dedicated atomic two-bit GC2 latch; neither threshold handoff nor sweep
  deferral reads or writes `GCSfinalize`.

The old marker implementation is still present but has no production caller;
the entry guard makes any accidental revival fatal in focused coverage. Its
physical deletion and removal of color-only fields remains cleanup work, not a
runtime or shutdown dependency.

## Remaining P0 GC2 completeness work

### Interned strings

GC2 currently has no runtime string-table reclamation domain. Shutdown now has
an exact GC2-owned terminal drain, while `lj_str_sweep_claim()` has no production
caller and would wait for active interners. GC2 still needs lock-free
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

A 2026-07-10 x86_64 `LUAJIT_USE_SYSMALLOC` probe makes the boundary explicit:
`collectgarbage("stop")` plus allocation and close succeeds through the new GC2
terminal drain, and the custom-allocator string resize/OOM close fixture passes;
allowing a normal automatic GC2 cycle still segfaults even for a small table
workload. This is a runtime ownership-domain failure, not a reason to restore
the old collector, and remains the next P0 implementation target.

### Exact arena object identity

The immediate correctness fallback is complete: every active-black JIT bump
table, function, and upvalue publishes through the per-TG pending-root chain.
The bump reservation itself remains wait-free and CAS-free in its existing
single-main-TG gate; active-black publication adds only exact pending-head/hint
stores. The scalable design remains per-arena exact object-start, type, extent,
and variable-cdata header-offset metadata. That metadata lets GC2 enumerate
destructors without retaining `gc.root` as a global ownership spine.

## Migration order

1. Land the per-arena identity sidecar to replace pending-spine traffic.
2. Add nonblocking runtime string-table sweep and the custom-allocation
   registry/allocator-generation protocol.
3. Delete the now-unreachable old marker, grey lists, color helpers and
   color-only `GCState` fields; retire the weak bridge after GC2 coverage is
   exhaustive.
4. Extend zero-entry and close/failure tests across custom allocators,
   sysmalloc, FFI finalizers, secondary TGs, Wine, and Darling.

The executable invariant is now “GC2 only”: neither normal execution nor
shutdown enters the retired marker/sweeper. Full collector completion still
requires runtime string reclamation, exact custom-allocation ownership, the
arena identity sidecar, and deletion of the unreachable compatibility code.
