# Interpreter Numeric FNEW Bump

`lj_func_newL_gc()` now shares the one-upvalue arena-bump helper used by the x64
JIT numeric `FNEW` helper, but only for the case where the normal cell-upvalue
path would allocate a fresh closed cell from a raw numeric local slot.

The predicate is intentionally narrow: exactly one upvalue, cell-upvalue proto,
local capture, and a numeric TValue still in the parent frame slot. The slot
address is formed only after the local-capture bit is known. The helper copies
the source `TValue` directly into the new upvalue so dualnum integer and number
representation stays the same as `func_celluv()`/`copyTVrel()`.

Mutable captures still publish the new upvalue back to the parent frame slot;
immutable captures do not. If the parent slot is already a `LJ_TUPVAL`, or if
the helper cannot prove the same local allocator/accounting conditions as the
existing bump fast paths, control falls back to `func_newL_gc_base()`. That
fallback is the owner of inherited upvalues, multi-upvalue protos, legacy
upvalue layout, existing-cell reuse, GC worker cases, custom allocators, and
MT-active allocation.

The active test coverage is behavioral: distinct loop-created closures must
have distinct upvalue identities, `debug.setupvalue()` must mutate only the
selected closure, two closures over an already-promoted mutable local must keep
sharing one cell, immutable numeric captures must read their snapshot values,
and the JIT direct helper keeps its accounting/fallback behavior. No source
shape guard is needed for this optimization.
