# Clocked `jit.attach` production test matrix (2026-07-19)

`tests/t-jit-attach-clocked.c` is the focused production regression for the
clocked exact-table transaction now used by `jit.attach`.  It is registered as
`m5_jit_attach_clocked` and runs under four build profiles:

- normal JIT and VM events;
- compile-time no-JIT;
- compile-time VM events disabled; and
- compile-time no-JIT plus VM events disabled.

The fixture uses the existing keyed-store post-CAS test hook.  For mapped
events it proves that the selected main-TG clock is odd during the semantic
CAS, every other lane is canonical and even, the cache mask is not invalidated
early, and the completed call exposes the matching even sequence/generation.
It also checks that every attempt releases its SMR reader, root descriptor and
TG root-anchor authority.

One deterministic BC-array generation handoff forces the first semantic CAS to
commit into a coherent old generation and return `STALE`.  The hook publishes
an already-built successor whose BC slot is nil.  `jit.attach` then resolves
and prepares afresh, performs a second exact CAS, and publishes a second clock
generation.  Both old and current slots contain the requested function at the
end, with exactly two hook hits and two even publications.  A separate resolver
injection forces a real Lua-stack relocation during an attach retry.

Public compatibility coverage includes:

- all five stock event hashes and clock lanes;
- the `"2694847501"`/`texit` collision and `"trace\0x"` full-length hash seed;
- unknown events and numeric event arguments formatted as `1.5`, `-0`, `nan`,
  `inf`, `-inf`, and `42`;
- same-value attach, ignored extra arguments, explicit-nil detach, runtime
  `jit.off`, and enabled/disabled argument-validation order;
- the established `name conflict for module '_VMEVENTS'` error with no mask or
  clock side effect;
- a registry `__newindex` diversion which captures the one table returned by
  `luaL_findtable` as an orphan while `_VMEVENTS` stays nil;
- throwing `__index` and `__newindex` methods on the event table, proving all
  attachment lookup/store/deletion is raw; and
- exact-function detach across mapped numeric, array, non-integral and negative
  numeric, string, boolean, table, lightuserdata, function, and full-userdata
  keys while preserving a different function and a non-function value.

A reachable `newproxy` runs a real close-time `__gc`, attaches and detaches a
TRACE function after terminal shutdown admission, and observes two canonical
even clock publications before `lua_close` completes.

Validation completed with:

```text
src/luajit tools/test.lua m5_jit_attach_clocked
```

All four profiles passed and the harness restored a normal default build.

Remaining focused-test debt is a deterministic concurrent resize/replacement
during the rooted detach iterator itself.  The current fixture covers its
actual-key deletion transaction and a generation change between prepared
attach CAS and confirmation, while the rooted iterator has separate structural
fixtures.  Public clock-exhaustion injection is also intentionally left to the
attachment-clock substrate test because reaching saturation through the Lua
API is not practical.
