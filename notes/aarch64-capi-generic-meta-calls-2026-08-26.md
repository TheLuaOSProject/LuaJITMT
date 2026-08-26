# ARM64 generic C API ownerless meta calls (2026-08-26)

## Scope

This follow-up migrates the five generic Lua 5.1 C API operations explicitly
left by the comparison/metatable tranche:

- `lua_concat`;
- `lua_gettable` and `lua_getfield`; and
- `lua_settable` and `lua_setfield`.

The implementation is target-neutral. Validation targets the native macOS
ARM64, GC64, disabled-JIT bootstrap.

## Complete ownerless boundary

The removed `api_vm_call_claimed()` protected only an actual metamethod call.
Its temporary-claim error edge released the target and then dereferenced and
rethrew on that unowned state; its success edge also left physical
`TG.cur_L` naming the target. Work before the call was not protected at all:
`lj_meta_cat`, rooted `tget`/`tset`, capacity growth, table waits, semantic
errors, and the `getfield`/`setfield` string allocation could all throw while a
stack-local temporary claim remained live.

Each migrated API now reserves caller error-carrier capacity before claiming
the target, without changing either stack top. Once an ownerless acquisition
is proven, it publishes the carrier on the distinct caller stack. All
allocation-, wait-, and error-capable preparation runs through
`api_vm_prepare_entry_claimed()`. This is the existing
`LJStateResumeBoundary` protocol generalized with the public API entry top as
its error checkpoint.

On a caught preparation or metamethod-call status, the exact error TValue is
copied and published into the caller carrier while target ownership is still
held. The target's public entry top, cframe, saved hint, owner word, TG root
depth, and physical `cur_L` are restored before only the temporary claim is
dropped. The status is then rethrown on the pre-captured caller. Already-owned
states continue through direct preparation and `lj_vm_call()` with no extra
claim or protected frame.

`lua_concat` repeats the protected preparation boundary for every reduction
step and retains the original API entry top throughout. Thus an error after an
earlier successful metamethod still restores the complete original operand
window. Getter/setter contexts retain stack offsets across growth and preserve
ordinary negative-index meaning through `getfield`/`setfield` key insertion.

## Validation

`tests/t-arm64-capi-generic-meta.c` deterministically covers all five APIs:

- ownerless metamethod success with a full collection inside each method;
- the legacy `lua_concat(L, n)` no-op for `n < 0`;
- ordinary negative table indices at every getter/setter entry;
- exact result/argument-consumption stack shapes;
- physical caller `TG.cur_L`, target owner word, saved hint, cframe, and TG
  root-depth restoration on success;
- a unique metamethod-body error for each API, transferred with its exact text
  while the target top is restored to the public entry offset;
- an actual-call STOPREQ through `lua_getfield`; and
- deterministic preparation-time allocation failure at the `getfield` and
  `setfield` key-allocation edge, reported as `LUA_ERRMEM`;
- foreign-owner rejection for all five APIs, with the caller and target tops,
  target owner/hint/cframe, and TG root depth unchanged; and
- successful reacquisition after every error.

Focused suite command:

```sh
env MACOSX_DEPLOYMENT_TARGET=13.0 \
  LJ_TEST_ROOT=/Users/frityet/Projects/LuaJITMT \
  src/luajit tools/test.lua m5_arm64_capi_generic_meta_calls
```

The focused suite passed with `arm64 C API generic meta calls: OK`. The full
native bootstrap regression gate, `tools/ci/arm64_bootstrap_gate.sh`, then
passed its 387-test Lua corpus and threading, hook, coroutine, signal/profile,
and FFI callback runtime gates. The new fixture is selected by the focused
command above; the full gate supplies the broader regression pass and restores
the ordinary native ARM64 no-JIT bootstrap afterward.

## Remaining limit

A caught error with no distinct protected caller state is still the legacy
terminal panic case. This tranche does not add a general longjmp cleanup
record or change that process-terminal contract.
