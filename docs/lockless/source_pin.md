# Lockless LuaJIT Source Pin

The lockless runtime plan was verified against this upstream source point:

- LuaJIT repository: https://github.com/LuaJIT/LuaJIT.git
- Branch: `v2.1`
- Commit: `b925b3e3fc6771171602323b45fbe9fb8fc90369`
- Ident: `LuaJIT 2.1.1780076327`

M0 imported the stock test cleanup suite from:

- Test repository: https://github.com/LuaJIT/LuaJIT-test-cleanup.git
- Commit: `014708bceb70550a3ab8d539cff14d9085ca9cb8`

The vendored `tests/stock/test/lib/contents.lua` expectations include a
small compatibility adjustment for the pinned LuaJIT v2.1 library surface
(`table.move` present, `math.mod` and `string.gfind` absent).

The M0 benchmark baseline is recorded under `bench/baseline_<host>.csv`.
