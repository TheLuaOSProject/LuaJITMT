# LuaJIT test launcher interpreter

- `tools/ci/lua_test.sh` no longer falls back to plain `lua` when no system
  `luajit` is available. If the in-tree binary is missing and `luajit` is not
  on `PATH`, the launcher builds `src/luajit` and uses that.
- This keeps stock-suite and semantics-oriented harness code under LuaJIT
  semantics by default, avoiding accidental Lua 5.4 execution on hosts that
  install `/usr/bin/lua` but not stock `/usr/bin/luajit`.
