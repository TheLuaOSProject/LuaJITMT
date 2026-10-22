# Source upvalue identity and traced frame ownership

Source captures marked `PROTO_UV_IMMUTABLE` previously copied the local into
separate cells without publishing the first cell back to its owner slot.
Consequently two closures over one read-only local had different
`debug.upvalueid` values, and `debug.setupvalue` changed only one closure.
Stock LuaJIT shares that local, including the owner's subsequent reads.

All source local captures now publish and reuse the owner's closed cell.
The immutability metadata remains available to the optimizer; it no longer
selects copy semantics. The normal constructor, bump paths, generic traced
helper, x64 inline allocation, recorder tracking, and failed-construction
cleanup use the same identity rule. Existing atomic TValue publication and
GC2 root barriers remain in place.

Debug writes through `debug.setlocal` and `lua_setlocal` must invalidate traces
that constified the promoted cell, just as `lua_setupvalue` already does.
Changed cell values use the existing trace-flush handshake before publication.
An identical value leaves traces intact. A failed flush drops the existing
state claim before raising the error. Raw unpromoted stack-local writes keep
their existing path; no new lock or admission authority is introduced.

## Inlined frames and stale stack storage

The wider source capture coverage exposed two recorder defects in the stock
recursive type-change test:

- Local-cell helpers used `REF_BASE` with a frame-local slot even inside an
  inlined call. They could overwrite a caller's local. Helpers now receive the
  current frame base, using the same `baseslot` offset as stack loads. At the
  root frame this is still exactly `REF_BASE`, preserving the x64 inline fast
  path. CNEW lookup also distinguishes different frames.
- A helper supplied with a current SSA value could ignore it and reuse an old
  cell still present in the physical stack slot from a previous invocation.
  Such a value now creates the fresh local's cell. Stack-sourced promotion
  retains the existing cell when one is already present.

These fixes keep recursive calls and sibling closures consistent without
disabling their compilation.

## Evidence and limits

- `t-upvalue-identity.lua` checks sibling identity, owner reads, Lua debug
  writes, distinct scope iterations, multiple captured values, upvalue joins,
  dump/load roundtrips, and sharing across an actual worker with GC. It passes
  with JIT enabled and disabled on ARM64 and x64/Rosetta. Installed stock
  LuaJIT passes the common portion in both modes. Frozen `709583ee` fails its
  first sibling-identity assertion.
- `t-debug-setlocal.c` exercises the C API and, on x64, requires a native
  getter trace. Same-value writes retain it; changed-value reads agree in the
  owner and both closures. Removing only debug invalidation reproduces the
  stale result.
- `t-jit-cell-frame.lua` checks recursive captures, fresh mutable locals, and
  caller sentinels without repeated debug flushes. It requires a native trace
  on x64. Removing the frame/fresh-value fixes reproduces a function value in
  the numeric depth slot.
- Assertion builds passed all 509 stock tests on experimental ARM64 and
  x64/Rosetta. The x64 focused recursive case passed ten consecutive runs and
  its complete stock suite passed five consecutive runs. The existing x64
  threading-upvalue fixture also passed.
- Safe ARM64 source-identity changes passed all 387 stock tests before the
  subsequent debug-flush and frame-address changes; a complete integrated
  bootstrap checkpoint is still required for those later edits.

The existing full x64 `t-jit-fnew-bump.c` fixture fails its SSB-space assertion
after an abort/reopen sequence on both unchanged `25efb19e` and this work.
Its publication-only mode passes. No assertion was weakened and no speculative
fixture drain was retained. This baseline discrepancy remains separate work.

Imported legacy bytecode and general ARM64 JIT admission remain separate
incomplete boundaries. Passing these source tests does not establish either.
