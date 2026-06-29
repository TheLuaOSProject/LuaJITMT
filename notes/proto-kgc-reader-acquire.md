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

Update: recursive bytecode writing now preserves the v2-loaded legacy-upvalue
marker in the current lockless dump format. Child prototype traversal still
walks prototype GC constants with acquire loads so release-published nested
prototypes are read coherently before their payload flags are emitted.

## Scope

This slice deliberately does not change the general `proto_kgc()` macro or JIT
recording users. Those call sites are more performance-sensitive and should be
handled as a separate pass with JIT-specific coverage if needed.
