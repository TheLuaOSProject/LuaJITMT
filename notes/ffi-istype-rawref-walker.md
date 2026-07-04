# FFI istype raw-ref walker

`ffi.istype()` uses a snapshot-local ctype comparator so it can compare types
without holding parser-owned pointers across waits. Its raw-reference helper now
matches `ctype_rawrefid()` by following both attributes and references in one
bounded loop before comparing the resolved raw type.

This is a helper invariant cleanup, not a stock-visible behavior change: stock
LuaJIT and the fork already agree that `int`, qualified `int`, `int &`, and
qualified reference typedefs compare as the same raw type for `ffi.istype()`.
The focused fixture now pins those reference/qualification cases alongside the
existing parser-token snapshot checks.

Coverage:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- stock `lib/ffi/istype.lua`
