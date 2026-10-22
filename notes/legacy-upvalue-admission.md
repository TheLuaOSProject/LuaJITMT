# Legacy open upvalues and secondary admission

Legacy bytecode can create an upvalue that points directly into its owner's
stack. Such a cell may move when the stack grows and changes representation
when the owner closes it. A secondary Lua thread must not observe those
changes concurrently. Source bytecode already uses closed cell upvalues and
does not need this restriction.

The supported policy is now explicit: the first `threading.spawn` or foreign
attach is rejected while any legacy open upvalue exists in the universe.
`threading.spawn` raises `cannot activate threading with open legacy upvalues`;
the integer-valued attach API returns failure. The check covers captures
reachable indirectly through tables, environments, or later channel messages,
including captures owned by suspended coroutines.

Rejection leaves every existing capture pointing at its original owner slot.
Owner writes and multiple closures sharing the cell keep ordinary Lua
semantics. After those upvalues close normally, the same closures may be shared
and activation succeeds. An unrelated source-only spawn is also rejected while
legacy opens exist elsewhere; this deliberate conservative boundary avoids
requiring a transitive heap-sharing analysis.

The existing legacy policy for captures created while entry is in progress or
after activation remains a closed value snapshot. This work does not claim
shared mutable-local semantics for legacy bytecode after activation. New
source bytecode retains its existing shared cell semantics.

## Exact authority and lifetime

One cold 32-bit `global_State.mt_openuv` word combines the irreversible
admission bit and the open-cell count. A capture reserves a count before it
publishes a new open upvalue; reusing an existing cell adds no count. The normal
close path releases the count only after copying the value, switching its
pointer, and publishing the closed cell. Allocation failure releases its
reservation, and count exhaustion fails allocation rather than changing capture
semantics. Source cell construction does not touch this word.

Secondary admission performs an acquire/release CAS from zero to the activated
state. A constructor racing that CAS either reserves first and vetoes entry or
observes completed admission and constructs a closed snapshot. They cannot both
win. This avoids a heap scan and the gap between separate entrant and count
checks. It uses native lock-free 32-bit atomics on ARM64 and x86-64.

Trace preparation runs before the irreversible CAS. Once the CAS succeeds,
there is no throwing operation before publishing `mt_active`. Closing's
release and admission's acquire also publish the now-immutable `GCupval.v` and
`closed` representation before the secondary VM can execute.

Normal returns, error unwinds, terminal coroutine GC preparation, state
destruction, and `lua_close` all close through `lj_func_closeuv`. Open cells
remain rooted on their owner's open list; closed-upvalue destructors must not
release a second reservation. The terminal coroutine fixture independently
checks the count remains held until the queued close has finished.

## Focused verification

All builds used disposable source copies, assertions, and macOS deployment
target 13.0.

- Native safe ARM64: expanded `t-legacy-upvalue-entering.c`,
  `t-gc-root-pending.c`, interpreted `t-threading-upvalue.lua`, and all 387
  stock tests passed.
- Native experimental ARM64 with GC2 test helpers: expanded legacy fixture,
  `t-thread-gcprep.c`, and all 509 stock tests passed.
- x86-64 under Rosetta with GC2 test helpers: expanded legacy fixture,
  `t-thread-gcprep.c`, the full `t-threading-upvalue.lua` including its JIT
  checks, and all 509 stock tests passed.
- The legacy fixture covers pre-entry owner identity, stack relocation,
  duplicate captures, clean spawn/attach rejection, successful attach/spawn
  after normal close, a shared closed capture surviving remote GC, count
  exhaustion, and 2,000 concurrent capture-versus-admission rounds.

The admission race fixture tests the exact authority independently; it is not
a substitute for a sanitizer or a complete weak-memory/model-checking proof.
The bounded ARM64 JIT admission policy is unchanged.
