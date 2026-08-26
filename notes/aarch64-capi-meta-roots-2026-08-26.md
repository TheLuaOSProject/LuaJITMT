# ARM64 C API comparison and metatable roots (2026-08-26)

## Scope

This tranche removes the raw C API generation gaps in `lua_rawequal`,
`lua_equal`, `lua_lessthan`, and `lua_getmetatable`, and closes the same
receiver/metatable gap in `luaL_getmetafield` and `luaL_callmeta`.

It does not change the Lua 5.1 API/ABI. The implementation is target-neutral,
but the motivating and validated configuration is the native macOS ARM64,
GC64, disabled-JIT bootstrap.

## Source-authoritative index capture

The old pair functions called `index2adr_read()` twice and retained the two
results in C locals. That was not a valid lockless capture:

- `LUA_GLOBALSINDEX` and `LUA_ENVIRONINDEX` both used `TG.tmptv`, so a distinct
  globals/environment pair could collapse into two pointers to the second
  value;
- registry and C-upvalue snapshots could outlive their mutable authoritative
  edge without an enumerated root; and
- adding a temporary root before resolving the second ordinary negative index
  would change what that index meant.

`ApiIndexRef` now resolves both original index descriptors before growth,
waiting, or a top change. A descriptor records a claimed stack offset, the
registry/upvalue edge, or the authoritative state/C-function environment edge;
it also preserves `LUA_TNONE` independently of a real nil TValue.

After a protected two-slot capacity check, each descriptor is transferred into
its own published natural stack root under source SMR and an exact value lease.
The pair roots remain live through raw comparison, rooted metamethod lookup,
and any metamethod call. Success paths restore the saved pre-operation top,
including after stack relocation. Invalid positive indices still compare false,
while two real nil values compare true.

`lua_equal` and `lua_lessthan` use `lj_meta_equal_rooted` and
`lj_meta_comp_rooted`, respectively. The ARM64 fast-pcall root-anchor checkpoint
from commit `79ffd1b3` is therefore a required companion: it rolls the helpers'
private `MetaChainRoots` back on caught semantic errors and STOPREQ unwinds.

## Raw metatable and auxiliary metafield paths

`api_getmt_raw_rooted` admits the exact object root before following its
replaceable metatable edge, acquires the exact metatable generation before
closing source SMR, and release-publishes the result into an enumerated stack
root before releasing either lease. Input and output may alias because the
object is overwritten only at a terminal, non-waiting publication point.

This helper is intentionally raw. `lua_getmetatable` must return the actual
metatable and must not apply `__metatable` hiding. An invalid index returns a
miss even if the nil base type has a metatable; a real nil value still exposes
that base metatable.

`api_getmetafield_key_claimed` now:

1. resolves and stack-roots the receiver before changing top;
2. transfers the raw metatable into a separate result root when the receiver
   must survive;
3. looks up the already-anchored string key with `lj_tab_gettv_rooted`; and
4. returns a naturally rooted exact result.

`luaL_callmeta` reserves three stack slots. They hold receiver, method/result,
and an overlap-safe scratch root. It publishes the receiver and method into the
final FR2 call layout before overwriting either old root, so no collectable
value is briefly live only in a C local. The non-FR2 rearrangement uses the
third root as a method handoff for the same reason.

## Deterministic validation

`tests/t-arm64-capi-meta-roots.c`, built with
`LJ_API_ROOT_TEST_HELPERS` and `LJ_TG_ROOT_TEST_HELPERS`, covers:

- real nil versus invalid-index behavior;
- a forced pair-API stack growth with both operands addressed by negative
  indices, followed by exact top-offset restoration;
- distinct `GLOBALS`/C-closure `ENVIRON` raw inequality plus shared `__eq` and
  `__lt` dispatch;
- registry versus C-upvalue capture;
- table and full-userdata equality, table comparison, and full GC inside the
  metamethods;
- raw nil base-type metatables;
- a one-shot publication hook which removes the receiver's only metatable edge
  and performs a full cycle after the metatable result root is published;
- the same edge removal during `luaL_getmetafield` and `luaL_callmeta`;
- successful ownerless-state calls and busy foreign-state rejection;
- semantic comparison errors with exact top/root-anchor restoration; and
- a synthetic STOPREQ from the rooted metafield boundary, proving fast-pcall
  rollback removes the generated-key anchor and temporary stack shape.

Focused validation command:

```sh
env MACOSX_DEPLOYMENT_TARGET=13.0 make -C src clean \
  'XCFLAGS=-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_DISABLE_JIT -DLUA_USE_ASSERT -DLJ_API_ROOT_TEST_HELPERS -DLJ_TG_ROOT_TEST_HELPERS'
env MACOSX_DEPLOYMENT_TARGET=13.0 make -C src -j8 \
  'XCFLAGS=-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_DISABLE_JIT -DLUA_USE_ASSERT -DLJ_API_ROOT_TEST_HELPERS -DLJ_TG_ROOT_TEST_HELPERS'
```

The helper-enabled native VM built successfully. The focused fixture completed
with `arm64 C API meta roots: OK`. The registered suite entry is
`m5_arm64_capi_meta_roots`.

## Remaining cross-file STOPREQ boundary

The current-state protected paths are covered, including semantic errors and a
synthetic STOPREQ. One narrower foreign-state boundary is not closed by this
`lj_api.c`-only tranche: if an ownerless target was resume-claimed by a C API
caller and an error is thrown before `api_vm_call_claimed` has a callable frame,
the target's stack roots unwind and `MetaChainRoots` roll back, but the
stack-local `LJStateClaim` is not itself an unwind action. This includes a fresh
STOPREQ from a `lj_tab_wait_l()` retry, anchor-reservation OOM, and a semantic
comparison error such as ordering two values without a valid metamethod.

A sound fix needs cross-file support rather than a naked `lj_vm_cpcall()` on a
suspended target (explicitly forbidden by the established state-claim design):

- either register an exact resume-claim cleanup record in the protected frame
  and consume it from `lj_err.c`, alongside the root-anchor checkpoint; or
- add nonthrowing, single-attempt rooted meta/table helpers so the API can drop
  the ownerless claim, service the retry/STOPREQ on the current error state,
  reacquire, and restart from freshly resolved authoritative descriptors.

Until that follow-up lands, the fixture validates ownerless success and busy
rejection, but does not claim ownerless pre-call STOPREQ cleanup.
