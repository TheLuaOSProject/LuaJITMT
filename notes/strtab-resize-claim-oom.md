# String table resize claim and OOM

Date: 2026-07-02

`lj_str_resize()` claimed the current string-table header before allocating the
replacement header. If that allocation raised `LUA_ERRMEM`, the claimed header
kept `LJ_STRTAB_RESIZE` set and later string interning could wait forever.

Resize now allocates and initializes the replacement header before claiming the
current header. After a successful claim it also verifies that the claimed header
is still `g->str.tabh`; if another thread already resized the table, it releases
the stale claim and either returns or retries only when the requested grow is
still needed.

`t-strtab-cas` now creates a Lua state with a failing allocator, forces the
replacement-header allocation to fail under a protected call, and asserts that
the current header is still unclaimed and subsequent interning works.
