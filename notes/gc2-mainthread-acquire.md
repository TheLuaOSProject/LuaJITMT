# GC2 main thread fallback acquire loads

## Context

Two GC2 helpers used `gcref(g->mainthref)` as a null check and then called
`mainthread(g)` to recover a `lua_State *` fallback:

- worker-pool startup allocation
- grey-stack growth when no current thread is available

`mainthref` is initialized during VM startup and then remains stable, but these
sites are runtime fallback reads in GC2 code. They were also double-reading the
same `GCRef` with plain loads.

## Change

Both sites now acquire-load `g->mainthref` once with `gcref_acq()` and derive
the thread pointer from that loaded object.

## Scope

This does not change the `mainthread(g)` macro globally. Most macro users run
after startup and treat the main thread as immutable process state. This slice
only removes the two GC2 runtime outliers that explicitly tested `mainthref`.
