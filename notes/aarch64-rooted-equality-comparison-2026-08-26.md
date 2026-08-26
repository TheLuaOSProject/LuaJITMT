# Apple ARM64 rooted equality and comparison (2026-08-26)

## Claim boundary

The JIT-disabled Apple ARM64 interpreter now performs distinct table/userdata
`__eq` lookup and ordered `__lt`/`__le` lookup from authoritative stack roots.
Lookup retries may relocate the Lua stack without leaving stale operand
addresses, and every selected method remains in an enumerated TG anchor until
the complete metamethod frame has been published.

This checkpoint deliberately adds ARM-only rooted entry points. The legacy
`lj_meta_equal` and `lj_meta_comp` ABIs remain in place for the C API and
non-ARM VMs, whose callers do not all provide authoritative `TValue` roots.
In particular, registry, upvalue, globals, and environment pseudo-indices from
`lua_equal`/`lua_lessthan` still need a separate two-root C-API capture path.
Raw `lua_getmetatable` remains the metatable-side C-API debt recorded in the
previous checkpoint. `ISNEXT` bytecode publication is still the last known
JIT-off interpreter mutation gap before ARM safepoint work can begin.

## Rooted equality protocol

ARM `ISEQV`/`ISNEV` no longer strips an object pointer and reads `metatable` or
the negative `nomm` cache inline. For distinct table/userdata operands it
passes both frame-relative stack slots to
`lj_meta_equal_rooted(L, lhsroot, rhsroot, ne)`.

The helper captures the pair inside one source-SMR interval and transfers both
exact leases into four owner-private TG anchors: left operand, right operand,
first method, and second method. Each `MM_eq` lookup may wait independently,
so all anchor addresses are reacquired after lookup. Lua 5.1 equality is
invoked only when both operands expose the exact same handler; a missing,
one-sided, or different handler preserves raw unequal semantics. Identity is
resolved without invoking `__eq`.

Once the handler and both operands are anchored, `mmcall` release-publishes the
method and argument slots. Only then does the helper advance `L->top` and drop
the anchors. The common ARM comparison return path reloads `BASE` from
`L->base` before interpreting the helper result or entering the metamethod
call, which covers stack relocation in any lookup retry.

## Rooted ordered comparisons

ARM comparison bytecodes now call `lj_meta_comp_rooted`; the old shared
`lj_meta_comp` implementation was restored unchanged. This split is required
because `lua_lessthan` may present pre-copied registry/upvalue values which are
not authoritative roots by the time a retrying helper opens SMR.

The rooted helper pair-captures the operands and keeps both candidate methods
in separate anchors. Lua 5.1 requires the exact same `__lt` or `__le` handler
on both operands. Lua 5.2-compatible builds retain their left-then-right
selection rule. When `__le` is absent, the helper preserves the specified
`not (rhs < lhs)` behavior by swapping only the logical anchor selection and
flipping the continuation condition; it never swaps or retains raw pointers.

Comparison type errors need special cleanup on ARM. x64 FR2 fast-pcall frames
carry a root-anchor checkpoint, but ARM fast `pcall`/`xpcall` frames do not.
The helper therefore copies only the two type-tag words, explicitly pops all
four anchors, and then calls `lj_err_comp`. Caught errors cannot strand private
roots or eventually exhaust the anchor chain.

## Native validation

A clean native ARM64 assert bootstrap with `LUAJIT_MT_ARM64_BOOTSTRAP`,
`LUAJIT_DISABLE_JIT`, and `LUA_USE_ASSERT` passed:

- the complete bootstrap gate, including all 387 vendored stock tests,
  threading API, hooks, coroutine handoff/dead-resume, and 320 FFI callback
  rounds;
- the strengthened source and emitted-object metamethod contract, including
  exact rooted relocations, stack-address ABI, post-helper `BASE` reload,
  rejection of raw equality metatable/cache reads, exact-handler/fallback
  structure, and error cleanup ordering;
- the focused metamethod runtime fixture (`baseline=3`, FFI `__eq` covered),
  including nested result/operand retention across full GC, identity/shared/
  different/one-sided equality, exact-handler ordered comparisons, reversed
  `__le` fallback, and exact TG anchor-depth restoration across 64 caught
  comparison errors;
- explicit Lua 5.1 semantic probes and 250,100 caught-error iterations each
  through `pcall` and `xpcall`, without anchor exhaustion;
- default and Lua 5.2-compatibility C compilation of the rooted helpers.

The current Lua 5.1 bootstrap does not support table `__len`; the runtime
fixture reports that path as unsupported instead of treating it as covered.
This does not affect the equality/comparison claim.
