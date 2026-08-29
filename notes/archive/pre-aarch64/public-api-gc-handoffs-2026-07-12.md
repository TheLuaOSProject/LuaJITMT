# Public API GC-valued handoffs (2026-07-12)

This tranche closes public Lua/C API windows where a collectable value was held
only in a C local while a target `lua_State` claim was released, reacquired, or
allowed a generation-following table lookup to yield. It does not change the
public LuaJIT API or ABI.

## Root handoff rules

- `lj_tg_root_anchor_reserve_nothrow()` ensures storage for the next TG anchor
  without publishing a root or raising. A string-producing operation can then
  return its `GCstr` and immediately publish it without an intervening allocator.
  Reservation failure is reported only after an ownerless target's tryclaim is
  released with `lj_state_dropclaim()`, not the resume-specific cleanup.
- TG anchor producers now reject `top >= LJ_ROOT_SCAN_LIMIT`. The shared remote
  scanner bound and producer bound are the same constant, so a published root
  can no longer be silently hidden past the scanner clamp.
- Constructor environment handoffs use one slot, not an independent cleanup
  root. The caller publishes `env` before releasing the target claim; the
  function, thread, or userdata constructor adopts that exact top slot and
  replaces it with the complete child. Existing constructor OOM paths pop the
  same slot before raising, including errors caught directly by Lua fast
  `pcall`/`xpcall`.
- A natural Lua stack slot is preferred when it is already part of the API's
  result or operand shape. This avoids anchor traffic on ordinary field access.

## Covered APIs

- `lua_pushlstring`, `lua_pushstring`, and `lua_pushvfstring` retain the new
  string from construction through target-state reacquisition and protected
  stack growth.
- Numeric `lua_tolstring`/`luaL_*lstring` conversion retains the formatted
  string through the compare-and-replace retry. C-upvalue trace flushing occurs
  only after dropping that temporary root, followed by a fresh retry, so an
  error cannot strand the root.
- `lua_getfield` uses its eventual result slot as the generated key root.
  `lua_setfield` materializes the same `|key|value|` stack shape as
  `lua_settable`. Metamethod calls and stale-generation waits therefore retain
  both operands naturally.
- `luaL_getmetafield` and `luaL_callmeta` retain the generated name in a TG
  anchor. Their pre-reserved result slot first roots the acquired metatable
  across `lj_tab_getstr()` generation following, then is replaced with the
  returned metamethod/value.
- `luaL_testudata` retains the generated registry key, the racy registry-table
  snapshot, and then the returned metatable snapshot before pointer comparison.
- `lua_getmetatable` returns a miss without growing; when a result needs a full
  stack it grows and then reloads the indexed object/metatable, so the earlier
  pointer snapshot never crosses protected growth.
  `lua_getfenv` already grew first; its cross-thread branch now publishes the
  environment into the destination stack while the source thread claim remains
  held.
- `lua_pushcclosure`, `lua_newthread`, and `lua_newuserdata` publish their
  captured environment before dropping a suspended target claim and transfer
  that same slot to the existing child constructor root.
- `lua_createtable` remains on the rooted table-constructor path. Its preclaim
  growth failure now uses tryclaim-specific cleanup and preserves a preexisting
  ownerless-state `tg_hint`.
- `luaL_newmetatable` is a protected local transaction. It roots the racy
  registry-table snapshot, key, and new table; keeps the key across stale waits;
  and transfers either the CAS winner or `LJ_TAB_STORE_CAS_EXISTS` snapshot into
  an anchor before target claim/growth. The nested `lj_vm_cpcall` unwinds all
  anchors before rethrowing, independently of Lua fast-pcall implementation.

The new-metatable path deliberately pays for nested protection because it is an
initialization/control-plane API. Common `getfield`/`setfield` paths add only a
pre-growth check and stack key publication. String pushes normally use the
embedded TG anchor block and do not allocate anchor metadata.

## Deterministic coverage

`m5_api_gc_handoffs` covers:

- full GC at generated-string and constructor-environment root publication;
- field `__index`/`__newindex` calls that collect while the generated key is on
  the real stack;
- suspended-state string and function/thread/userdata construction;
- forced 17th-slot reservation OOM with owner and `tg_hint` baseline checks;
- the exact one-million-slot producer boundary without allocating one million
  objects; and
- a deterministic `luaL_newmetatable` peer-wins CAS. The hook removes the
  competing table's desired and temporary registry edges and forces a full
  cycle after `old` is rooted, proving that the returned table no longer relies
  on a stale registry snapshot. The competitor constructor root is transferred
  to the temporary registry edge before entering the transaction, so every TG
  anchor remains strictly LIFO.

## Immediate follow-up: lockless lightuserdata segments

`lj_lightud_intern()` remains a separate, bounded release issue. Its current
growable `gc.lightudseg` array and plain `lightudnum` publication allow duplicate
segment IDs, lost appends, and readers racing a realloc/free.

The recommended next slice is a fixed 255-entry map (segment 255 stays reserved
for internal TValue sentinels), initialized with an impossible upper-pointer
sentinel and never resized before universe close. Use deterministic open
addressing and release CAS per slot: equal upper bits converge on the same
bucket, different bits probe without a global lock, and `lightudV()` performs one
acquire slot load. This removes realloc SMR entirely and bounds memory to about
1 KiB per universe. It was intentionally not mixed into this API/string commit
so its TValue encoding and close-path tests can be reviewed independently.
