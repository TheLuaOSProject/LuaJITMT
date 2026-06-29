Threading API boundary
======================

The public C API in `lua.h` should stay stock LuaJIT. The lockless threading
runtime still needs internal attach/detach hooks for VM-owned helpers, FFI
callback carriers, and white-box concurrency fixtures, but those hooks live in
`lj_thr.h` as `lj_threading_attach()` and `lj_threading_detach()`.

Do not add fork-local `lua_*`, `luaJIT_*`, `luaopen_*`, or `LUA_*LIBNAME`
symbols for threading convenience. The `threading` opener is hidden and
registered only through the VM's internal preload table. Lua-facing experiments
belong behind `require"threading"`; C fixtures that need to attach a foreign
pthread are testing internals and should include the internal header explicitly.
