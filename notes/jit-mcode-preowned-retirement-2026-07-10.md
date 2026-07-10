# Preowned machine-code retirement nodes (2026-07-10)

## Problem

The full trace-flush boundary owns the global recorder token and may execute as
the safepoint leader.  Its mutation phase must therefore be non-throwing.
`lj_mcode_free()` previously allocated one `MCodeRetire` node per executable
area with `lj_mem_newt()` after trace bodies, bytecode links, and exit metadata
had already been retired.  An allocator failure could longjmp out of that
partially completed flush while retaining the recorder token.

## Representation

Every machine-code area now acquires its `MCodeRetire` sidecar in
`mcode_allocarea()`, while trace assembly is already an explicitly fallible
transaction.  Active sidecars are published on `jit_State.activemcode` and are
marked by the GC2 JIT-root scan.  A distinguished active epoch prevents an
active owner from being confused with a valid epoch-zero retirement.

`lj_mcode_free()` performs only a no-throw ownership transfer:

1. exchange the active sidecar list to `NULL`;
2. release-publish the current retirement epoch in every node;
3. preserve and CAS-publish the batch on `retiredmcode`;
4. detach the active executable-area chain.

Finite `LJ_FLUSH_EPOCHS` reclamation and retired-trace mcode-reference checks
are unchanged.  Shutdown drains both the active owner list and retired list.

The sidecars deliberately remain ordinary writable arena allocations instead
of being embedded in executable mappings.  Thus list publication and epoch
updates require no RX-to-RW transition on Windows, do not toggle macOS
`MAP_JIT` write protection, and use the existing Linux/x64 dual-map behavior
without writing through its executable alias.

## Failure cleanup and performance

Both fallible resources remain local until a complete owner pair exists.
`mcode_alloc()` now returns allocation failure to `mcode_allocarea()`.  After a
mapping succeeds, the new internal `lj_mem_new_nothrow()` obtains the sidecar
without a longjmp; if that fails, `mcode_allocarea()` unmaps the unpublished
area before raising the normal JIT machine-code allocation error.  No mcode or
list state has been published at either failure point.

Allocating the mapping first also matters to concurrent GC: the potentially
blocking/native OS allocation happens before the raw arena sidecar exists.
Once allocated, the sidecar is marked and published without another native or
blocking interval.

The total number and size of sidecar allocations are unchanged; allocation is
merely moved from flush time to the already-cold area-allocation path.  Flush
loses its allocation loop and performs a single list exchange plus the same
linear epoch stamping, so its steady-state cost is no worse.

## Regression coverage

`tests/t-jit-mcode-retire.c` now verifies both active ownership and the retired
epoch transition.  It installs an allocator that rejects every growth while
calling the eventless full flush and asserts that the flush makes zero growth
calls.  The existing finite-grace and retained-trace mcode-reference checks
continue to cover delayed reclamation.

Focused Linux/x64 validation:

- default source build;
- clean `LUA_USE_ASSERT` + `LJ_GC2_PARANOIA=1` source build and fixture;
- `t-jit-mcode-retire` with warnings-as-errors;
- the eventless flush while all allocator growth is denied (100 repeated runs);
- the focused fixture under Valgrind with definite-leak checking.

The changed allocator/mcode translation units also compile for x86_64 Windows
with MinGW and for macOS 11 x86_64 with osxcross (selecting the `MAP_JIT`
configuration).  Platform execution remains part of the combined Wine/Darling
gate rather than this focused representation test.
