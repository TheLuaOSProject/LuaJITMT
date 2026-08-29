# Stock Error Location Semantics, 2026-07-06

The vendored stock suite caught a regression where fast-function argument
errors kept the stock type/name text but lost the leading `file:line:` source
decoration. Example before the fix:

```
bad argument #1 to 'abs' (number expected, got no value)
```

Stock LuaJIT includes the call site:

```
(command line):1: bad argument #1 to 'abs' (number expected, got no value)
```

Root cause:

- `lj_debug_addloc()` had been hardened to require `frame_islua(frame)` before
  deriving a source line.
- In this fork, fast-function/helper-backed argument errors can format while
  the candidate caller frame has a valid Lua function slot but a non-Lua frame
  marker.
- The strict marker check therefore skipped optional source decoration even
  though the function slot and predecessor PC were valid.

Fix:

- Keep the frame-slot validation that protects trace/helper error formatting.
- Do not require the frame marker itself to be Lua before checking
  `isluafunc(frame_func(frame))`.
- Let `debug_framepc()` reject frames that cannot safely yield a bytecode
  position.
- Route the `m3_interp_stock_joff` test through `runtime.run_stock()` and make
  stock runner paths absolute so tests still work after changing cwd to
  `tests/stock/test`.

Verification:

- Exact stock argcheck files:
  `lib/math/abs.lua`, `lib/string/len.lua`, `lib/string/sub.lua`.
- `src/luajit tools/test.lua m3_interp_stock_joff run_stock_tests`
- `src/luajit tools/test.lua m0_build_profile_helpers m5_stock_api_surface`
