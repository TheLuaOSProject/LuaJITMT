# Function and loader construction anchors (2026-07-12)

## Problem

`lj_func_newL_empty()` previously linked a READY Lua closure before allocating
its closed upvalues. The closure then existed only in a C local until the
caller stored it on the Lua stack or in a library table. A concurrent GC2 cycle
could retire it between any of those allocations. A thrown allocation also
left no precise caller-visible construction-root unwind contract.

The source and bytecode loaders had the same handoff gap for their top-level
prototype. The direct generated-library bytecode path additionally had no
protected cleanup for TG root anchors on malformed input or OOM.

## Implemented contract

- Source parsing and bytecode loading return a top-level `anchoridx`. The slot
  contains the complete prototype on return.
- `lj_func_newL_empty(L, pt, env, anchoridx)` requires that prototype anchor.
  It builds the closure and all closed upvalues as a private READY=0 chain with
  non-throwing allocations. On failure it frees the exact private chain before
  throwing, leaving the original prototype anchor unchanged.
- Every `uvptr` is initialized before a release store publishes the exact
  physical `nupvalues` count. This makes destructor sizing exact even during
  cancellation and pairs with acquire readers.
- On success, the constructor replaces the prototype anchor with the function
  while the function is still opaque, performs READY plus root-barrier
  publication, publishes each child through a temporary semantic anchor, and
  finally publishes the private ownership chain.
- `cpparser` keeps that function anchor until its result stack slot is release
  published. Non-native bytecode keeps the prototype anchor until the prototype
  stack slot is published.
- Generated library Lua functions retain both their generated name and their
  prototype/function handoff in TG anchors. A local protected call restores the
  complete anchor baseline before propagating any error.
- `lua_loadx` snapshots and restores its TG anchor baseline around its protected
  parser call. `LexState` is zero-initialized so cleanup is valid even when the
  protected entry fails before lexer setup.

## Runtime FNEW paths

Multi-allocation runtime constructors now build unpublished function/upvalue
chains with non-throwing allocations and exact cancellation. They publish the
chain only after all fields are initialized, then issue a semantic root
publication before returning to the VM. Mutable local-cell publication uses a
release TValue store. The prototype's saturating closure-count heuristic uses a
relaxed byte CAS in the generic multi-TG path, eliminating lost increments and
plain-store races; bump/JIT updates retain their cheaper form only under the
single-producer admission gate.

Fresh C closures reserve a TG construction anchor before allocation. Their
nil-initialized body becomes READY under that durable root, and every caller
retains the anchor while filling upvalues and establishing the final
stack/table edge. Protected generated-library registration drains all such
anchors on error; successful API/library paths pop the closure anchor only
after the final edge and its publication barrier. Function-kind readers use an
acquire `ffid` accessor; generated-library final `ffid`/PC changes are
release-published, including `lj_lib_pushcc` finalization after its helper has
placed the provisional C closure on the owned stack.

Generated-library `lastcl`, copied values, and freshly interned string
constants now use release stack stores followed by the stack publication
barrier. This gives non-function GC values in the same constructor loop the
same pre/post handoff coverage before a later record pops their source edge.

Arena bump helpers run only in the single-producer allocator window and expose
their block bit after the complete header is initialized. If an allocation
would flush accounting, the helper declines the bump path before consuming its
window; the generic READY=0 allocator handles that case. A successful bump
handoff therefore performs no GC-capable assist after READY and pays no extra
root barrier on ordinary local accounting.

Interpreter `BC_CNEW` and `BC_FNEW` now route their completed stack stores
through the VM root-publication helper. The store's dirty-epoch bump alone is
not a sweep-rescue linearization point when allocation accounting completed a
snapshot while the object was opaque.

JIT GC-valued promotion uses `IR_TMPREF`, materialized in `TGState.tmptv` (or
`tmptv2`). GC2 acquire-scans both fields whenever `jit_active` is published.
FNEW additionally syncs raw trace values into `BASE` before entering its
constructor. Thus the input remains a durable TG/stack root across snapshot
allocation; a traced table-valued promotion stress test covers this ABI.

## Bytecode body construction

Bytecode prototypes remain READY=0 through bytecode verification, upvalue data,
all KGC/numeric constants, and debug data. The prototype anchor is visible but
opaque during that interval. Each KGC gets its own retained TG anchor until the
complete parent reaches READY and its root barrier transfers the child leases.
Template-table construction uses a table anchor plus reusable key/value anchors
spanning parsing and resize-capable stores. Each array/hash store then publishes
its value (and hash key) to the already-READY table, closing a traversal which
scanned the initial nil slots.

Each prototype body is read under a small protected cancellation wrapper. A
malformed body or OOM frees the exact unpublished `sizept` allocation before
dropping its operand anchors; otherwise READY=0 would deliberately pin the
abandoned body forever. A repeated mutated-dump test checks that a warmed second
pass does not accumulate prototype bytes. The reader now records the declared
end of each body, bounds-checks byte/block/ULEB reads against it, and performs
wide checked size arithmetic before allocation, so hostile internal lengths
cannot overrun the body before the final exact-length check.

## Test coverage

`tests/t-func-construction-anchor.c` uses `LJ_FUNC_TEST_HELPERS` to fail each
closed-upvalue allocation deterministically. For every failure ordinal it
checks:

- `LUA_ERRMEM` is propagated;
- the temporary child anchor has been removed;
- the caller's prototype anchor remains exact;
- the TG anchor depth is unchanged apart from that caller anchor; and
- `gc.total` returns to the pre-construction value, proving exact body cleanup.

The existing source/bytecode round-trip and FNEW fixture gates remain the broad
behavior and publication-order coverage.

The open-upvalue failure case uses a test-only hook to complete a major cycle
immediately after the first new open upvalue has reached its per-state list
root, then fails the second allocation. This deterministically covers the
post-READY root handoff rather than relying on allocator accounting to schedule
collector progress.

## Legacy open-upvalue ring retirement

The shared `global_State.uvhead` doubly-linked ring is no longer runtime
topology. Its four-store insertion/unlink protocol could corrupt unrelated
states when legacy bytecode opened or closed upvalues concurrently, and GC2
does not consume it: owner/root scans already traverse each lua_State's
release-published `openupval` chain. Runtime creation and close therefore use
only that per-state chain, then transfer a closed cell to pending ownership.
The `uvhead` field remains a self-linked, otherwise untouched compatibility
sentinel so the global-state layout does not change.

A repo-wide `uvhead|uvprev|uvnext` audit leaves only that two-store sentinel
initialization in `lj_state.c`, dormant layout/accessor definitions in
`lj_obj.h`, and the independent build-time `host/minilua.c` interpreter. There
are no target-runtime open/close readers or writers of the shared ring.

The focused constructor fixture forces a current-format child prototype down
the legacy open-upvalue path, attaches four independent coroutines to four OS
threads, and repeatedly opens/closes two captures concurrently. Automatic GC is
suppressed during the topology race so unrelated simultaneous safepoint
handshakes cannot dominate it; a full major collection after detach verifies
free/reuse. Every per-state chain must drain and the dormant compatibility
sentinel must remain self-linked.

`tests/t-bcdump-current.c` also loads a table-constant dump through 17-byte
reader chunks and completes a full major GC before every chunk, forcing cycles
through prototype body and template-table construction.

`m5_function_construction_anchors` builds the core and focused C fixture with
both `LUA_USE_ASSERT` and `LJ_FUNC_TEST_HELPERS`, so deterministic cancellation
and anchor-depth checks run in the normal suite registry.
