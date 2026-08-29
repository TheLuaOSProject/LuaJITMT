# GC2 parser native roots and prototype handoff (2026-07-12)

## Problem

`LexState` is a native stack object, but source and bytecode loading can span
many GC2 cycles. Its `bcstack`, `vstack`, and token/debug `SBuf` backing are raw
allocator objects. Before this change no durable root named those allocations:
the owning C frame was invisible to a GC2 root scan. A collection entered from
a reader, parser allocation check, or VM event could therefore recycle live
parser storage.

Parser semantic temporaries had two adjacent gaps:

- the chunk-name and per-function constants table were written to `L->top`
  without the stack-root publication barrier; and
- `fs_finish()` relied on the ownership spine for the completed source proto.
  The BC VM event can run arbitrary Lua and GC after the proto is linked, and
  the caller does not install the parent constant/output edge until the event
  returns. Ownership is allocation/destructor reachability, not a semantic
  root, so that interval was not closed.

## Owner-published LexState descriptor

Each `TGState` has an acquire/release `lexstate` head. `lj_lex_setup()` fully
initializes the descriptor fields and release-pushes its `LexState` before the
first reader call. This ordering matters because a reader can re-enter Lua,
start a nested load, collect, or throw before normal setup returns.

The descriptor is a LIFO:

- `root_prev` closes nested/reentrant loads on the same claimed TG;
- `root_bcstack` and `root_vstack` are release-published duplicate bases,
  updated after every successful vector relocation; and
- the SBuf base is read through its existing acquire accessor because
  `lj_buf_bounds_rel()` already release-publishes every relocation.

`lj_lex_gc2_markroots(g, tg)` is called by the owner-TG root scan and marks all
three exact raw allocation bases for every active descriptor. Owner root scans
run at that TG's stopped acknowledgement/claim boundary, so a C-stack
descriptor cannot disappear concurrently with its scan. This avoids a global
lock, wait, heap descriptor, or reclamation grace on every load.

Cleanup uses the inverse order. It verifies LIFO ownership, release-restores
the preceding TG head, clears the published duplicate bases, and only then
frees the three raw allocations. Protected setup/parser errors are covered by
`lua_loadx()`'s unconditional lexer cleanup. Cleanup also leaves zeroed buffer
state, making a duplicate terminal cleanup harmless. TG detach treats a live
descriptor as an internal scope leak and fails closed instead of publishing a
dangling native root.

The descriptor is intentionally per-TG rather than global. It preserves the
lockless owner-private parser fast path and makes a root scan proportional to
the nesting depth of active loads, not to historical loads or all VMs.

## Semantic parser publication

Source parsing now publishes both ordinary stack anchors with
`lj_state_stack_pubtv()`:

- the interned chunk name; and
- every `FuncState.kt` constants table.

`fs_finish()` reserves a TG semantic-anchor slot before allocating its pending
proto. After all colocated arrays and header fields are initialized it:

1. release-stores the proto TValue into the anchor while READY is still clear;
2. publishes header READY;
3. applies the active root barrier to the anchor;
4. publishes ownership; and
5. emits the BC VM event while the anchor remains live.

For a nested function, `parse_body()` pops the anchor only after `const_gc()`
has installed the proto in the parent constants table. For the top-level
function, `lj_parse(ls, &anchoridx)` returns the still-live exact anchor to the
loader. The loader replaces/retains that anchor through `GCfunc` construction
and final Lua-stack publication, then pops it. The protected-load anchor-base
drain handles every error path, including an erroring BC event.

This intentionally diverges from treating `g->gc.root`/pending ownership as a
semantic constructor root. The split is required for GC2: ownership keeps a
body discoverable for destruction, while a semantic anchor proves the object
graph is live and must be traversed in the current cycle.

## Verification

Focused coverage is in:

- `tests/t-parser-gc2-roots.c`: exact raw marking, nested descriptor LIFO,
  cleanup idempotence, setup-reader nonlocal unwind, and post-error reuse;
- `tests/t-parser-gc2-roots.lua`: large bytecode/variable/debug buffers,
  chunked nested readers, forced full collections, BC VM events, repeated
  syntax errors, setup-reader errors, and successful reuse afterward; and
- `m5_parser_gc2_roots`, which runs both tests with internal assertions.

The raw buffers still use the runtime allocator and its normal accounting.
The repository's separately documented temporary custom-`lua_Alloc` omission
therefore remains unchanged by this slice.
