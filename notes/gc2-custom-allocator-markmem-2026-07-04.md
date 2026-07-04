GC2 custom allocator raw-memory marking
======================================

`m5_strtab_cas` exposed an intermittent segfault in the resize-OOM coverage
that builds a state with a custom `lua_Alloc`. ASan made the failure
deterministic: during `lua_newstate`, table hash-vector publication called
`lj_gc2_markmem()` on memory returned by the custom allocator. The old GC2 path
derived `lj_arena_of(p)` and loaded `owner_tid` before proving the allocation was
arena-backed, so plain `realloc()` storage could be read before its allocation.

The fix is to make GC2 raw-memory bitmap marking conditional on the atomic
`global_State.allocf_arena` mode bit. Custom allocator storage has no arena
bitmap to mark, and stock LuaJIT semantics allow embedders to supply a custom
allocator. The mark path now no-ops for non-arena states instead of probing for
an owner header.

Verification:

- ASan `t-strtab-cas` loop: 100/100 passed after failing immediately before the
  guard.
- Clean `tools/ci/lua_test.sh m5_strtab_cas`: passed.
