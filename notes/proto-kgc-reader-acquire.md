# Prototype GC constant reader acquires

## Context

Prototype GC constants are filled with release stores by the parser and bytecode
reader:

- `lj_parse.c` stores GC constants with `setgcrefrel()`.
- `lj_bcread.c` stores bytecode-loaded GC constants with `setgcrefrel()`.

Several reader loops still loaded those slots with plain `gcref()`:

- `bcwrite_kgc()`
- child prototype recursion in `bcwrite_proto()`
- `snap_useuv()`

These paths do not mutate the constant array, but they do consume object
pointers from release-published prototype storage.

## Change

The explicit loops above now use `gcref_acq(*kr)` when reading prototype GC
constant slots.

Update: `bcwrite_has_legacyuv()` remains part of the bytecode compatibility
path. It walks prototype GC constants with acquire loads so v2-loaded prototype
trees are not silently re-emitted as current lockless bytecode.

## Scope

This slice deliberately does not change the general `proto_kgc()` macro or JIT
recording users. Those call sites are more performance-sensitive and should be
handled as a separate pass with JIT-specific coverage if needed.
