# Stock compatibility audit, 2026-06-29

Scope: active `v2.1` commits through `767ec6f911aa`, with emphasis on
source-search test removal, legacy/fork-local entrypoint cleanup, and stock
LuaJIT API behavior.

Refresh note: the 2026-06-29 re-audit after removing the non-stock `ffi.pin`
entrypoint, and again after adding shared-cdata race coverage, found no active
source-search-only tests and no public stock LuaJIT API removals. The remaining
string checks in active tests are over generated dumps or runtime output,
matching the policy exception for generated ASM/mcode and other generated
artifacts.

## Source-search tests and CI

No active forbidden source-search-only tests were found under `tools/ci`,
`tools/test.lua`, `tests/suites`, `tests/lib`, top-level `tests/*.lua`, or
top-level `tests/*.c`.

Allowed remaining searches are over generated artifacts or runtime output: JIT
dumps, bytecode listings, generated mcode/ASM dumps, captured stdout/stderr,
CSVs, and marker files. Those checks are permitted because the generated
artifact is the behavior under test. Repository source checks for helper names,
field accesses, function calls, or snippets are not permitted.

The removed source-guard compatibility surface remains gone:

- No `m0_source_guard` case or per-case shell wrapper exists.
- No `suite_utils.read_source_file()` helper exists.
- The only `tools/ci` shell entrypoint is `tools/ci/lua_test.sh`.
- The remaining build-time `find`/`sed`/`grep` invocations inspect generated
  dependencies, object files, or install-metadata output, not repository source
  snippets.

## Public C API surface

The current local `upstream/v2.1` reference was checked as
`a2bde60819d83e6f75130ac2c93ee4b3c7615800`. Public headers do not add or remove
stock C API prototypes. `src/lualib.h`, `src/lauxlib.h`, and
`src/luajit_rolling.h` match upstream. `src/lua.h` differs only by one blank
line. `src/luaconf.h` differs only in comment wording.

Fork-local removals are not stock API removals:

- `luaMT_spawn`, `luaMT_join`, `luaMT_fence`, `luaMT_attach`, and
  `luaMT_detach` were fork-local threading C APIs. Their public declarations
  were removed; internal attach/detach lives in `lj_thr.h`.
- `luaopen_threading` and `LUA_THREADINGLIBNAME` are intentionally hidden from
  public headers. The Lua extension remains `require("threading")`.
- `ffi.pin` was an unsupported fork helper, not stock LuaJIT FFI. Ordinary Lua
  references and `ffi.gc` remain the stock-visible lifetime mechanisms.
- `LUA_GCGENERATIONAL` and `LUA_GCINCREMENTAL` are not stock LuaJIT public C
  constants in this branch. Fork GC mode control is `threading.gcmode()`.

Export audit: `luaopen_threading`, `luaMT_*`, `lj_threading_*`, and
`ffi_typeinfo` are not dynamic public exports. `ffi.typeinfo(id)` is restored as
the stock LuaJIT 2.1 internal/unsupported Lua-visible FFI diagnostic, with
lockless snapshot semantics so it does not expose active parser rollback state.
`ffi_pin` and its handle userdata type are removed. The remaining C
attach/detach entry points are internal helpers in `lj_thr.h` for native
attached-thread fixtures and runtime integration.

## Stock behavior boundary

Single-threaded stock behavior should remain the default compatibility target.
Threading-only deviations are documented rather than enforced with source
searches:

- Explicit GC keeps stock behavior when no secondary Lua thread is live. With
  live secondary Lua threads, `lua_gc(L, LUA_GCCOLLECT, ...)` uses the GC2
  active-thread collector path, and `LUA_GCSTEP` requests/assists GC2 work
  instead of running the legacy single-thread full-GC path.
- `require("threading")` is a Lua-visible extension and may occupy
  `package.preload.threading`; it must not become a public C API symbol.
- Bytecode compatibility accepts the intended lockless v2/v3/v4 compatibility
  surface, while current dumps remain fork-format v4 with the legacy-upvalue
  payload flag.
- `os.setlocale()` mutation after threading activation is rejected because
  process-global locale changes are not safe with active VM threads.
- `math.random()` and `math.randomseed()` use per-thread-group PRNG state once
  the threading extension creates separate VM thread groups. The single-main-TG
  path keeps ordinary stock-facing library calls available.
- `jit.profile` remains stock before threading activation; unsupported
  non-TG-local profiler backends may drop samples while multiple VM threads are
  live.

The `m7_ffi_cdata_shared_hammer` coverage added after the pin removal is stock
API coverage: it uses `ffi`, `require("threading")`, and ordinary cdata field
accesses. It intentionally does not add a source-search test for helper names.

Past cleanup commits temporarily removed stock compatibility symbols and the
optional `LUAJIT_ENABLE_LUA52COMPAT` profile. Current branch state restored
those stock surfaces; `tests/t-stock-api-surface.c` covers representative
legacy stock C API entry points without searching implementation source text.

Future legacy cleanup should remove only stale fork-local compatibility shims
that exist for the threading/lockless migration. It should not remove stock
LuaJIT C API symbols, stock library options, standard FFI behavior, or stock
single-threaded behavior.
