# Threading State Registry Validation

`global_State.threading_states` is a raw lockless list of non-main `lua_State`
objects. Removal, close-time freeing, legacy GC root scans, and GC2 root scans
were loading `thread_next` and other state fields before proving that the
registry entry still named a registered thread object.

This slice adds `lj_state_thread_registry_valid()` and uses it to require a
queued, live `LJ_TTHREAD` object before dereferencing a registry entry.

Follow-up: registry head and link publication now route through
`lj_state_thread_registry_head_*()` and
`lj_state_thread_registry_next_*()` helpers. The publish, remove, close-time
free, legacy GC scan, and GC2 scan paths no longer spell raw pointer
load/CAS/xchg operations against `global_State.threading_states` or
`lua_State.thread_next`.
Follow-up verification: clean build, `m4_threading_live_root`,
`m4_threading_join_gcscan`, `m4_threading_shutdown`, and `m3_gc2_scaffold`
passed.

Guarded paths:

- `state_registry_remove()`;
- `close_state_free_registered_states()`;
- legacy `gc_mark_threading_states()`;
- GC2 `gc2_scan_threading_states()`.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m4_threading_live_root`
- `tools/ci/lua_test.sh m4_threading_join_gcscan`
- `tools/ci/lua_test.sh m4_threading_shutdown`
- `tools/ci/lua_test.sh m3_gc2_paranoia`
- `tools/ci/lua_test.sh m0_matrix`
