Nested lua_State TLS ownership
==============================

Context
-------

`ffi.C.luaL_newstate()` can create a second independent `global_State` while
running inside an existing LuaJIT VM. The nested state is not part of the outer
VM's thread group. Treating the OS-thread TLS `TGState` as globally valid caused
the nested VM to borrow or overwrite the outer VM's thread-group state.

Fix
---

* `lj_thr_get_tg_fallback(g)` now only returns the TLS `TGState` when it belongs
  to the requested `global_State`; otherwise it falls back to `g->main_tg`.
* `lj_tg_init()` only seeds OS-thread TLS when TLS is empty, so creating a
  nested VM from inside an existing VM does not steal the outer VM's TLS owner.
* `lj_tg_cur_L(g)` and `lj_tg_jit_base(g)` use the thread group attached to the
  requested `global_State`, not whichever unrelated thread group happens to be
  in OS-thread TLS.
* `close_state()` clears TLS when closing the VM that currently owns TLS, before
  freeing the main thread group.

Validation
----------

* `tools/ci/lua_test.sh m7_ffi_nested_state`
* `tools/ci/lua_test.sh m7_ffi_clib_ldscript m7_ffi_clib_cache m7_ffi_nested_state`
* `tools/ci/lua_test.sh m3_safepoint_handshake m4_threading_api m4_threading_smoke`
* `(cd tests/stock/test && ../../../src/luajit sysdep/ffi_lib_c.lua)` no longer
  crashes in the nested `luaL_newstate()`/`lua_close()` path. It now reaches the
  stock test's final `ffi.load("pthread")` probe, which fails in this container
  because `libpthread.so` is not available as a loadable soname.
