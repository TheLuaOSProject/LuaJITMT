# `lua_getupvalue()` acquire snapshot

Upvalue writers (`lua_setupvalue()`, C closure construction, and upvalue join
paths) publish shared upvalue cells with release stores. `lua_getupvalue()` still
copied the selected cell directly into the caller stack result slot.

Changed `lua_getupvalue()` to acquire-load the selected `TValue` with
`lj_tv_load_acq()` before incrementing `L->top`. This covers both Lua upvalue
cells returned by `lj_debug_uvnamev()` and C closure upvalue cells. The
`debug.getupvalue()` stack reshuffle remains a local stack copy after the API
has already acquired the cell value.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m5_upvalue_publish_gc.sh`
- `tools/ci/m4_threading_upvalue.sh`
- direct `debug.getupvalue()` / `debug.setupvalue()` smoke
