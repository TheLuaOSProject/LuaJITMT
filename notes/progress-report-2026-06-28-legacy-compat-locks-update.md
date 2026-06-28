# Progress report - 2026-06-28 legacy/compat and lock cleanup update

Scope: x86_64/Linux `v2.1` lockless LuaJIT fork. Policy remains safety,
stability, and language semantics over chasing LuaJIT performance at any cost.

## Current estimate

Overall correctness/stability progress: 68-78%.

- Runtime lockless substrate: 73-83%.
- Legacy/compat public/runtime surface removal: 82-88%.
- CI migration away from shell aliases and stale source guards: 68-78%.
- FFI concurrency outside mutable `ffi.cdef`: 65-75%.
- FFI recorder read-only ctype paths: 80-88%.
- Interpreter-side FFI parser fallback removal: 45-55%.
- Release-quality soak and benchmark readiness: 45-55%.

Time remaining forecast:

- Correctness alpha: about 2-4 focused weeks.
- Strong beta with broader FFI/JIT/GC stress: about 6-10 focused weeks.
- Performance cleanup after semantic closure: about 4-10 focused weeks.
- Production-confidence soak: about 3-6 months of workload validation.

## Done in this update

- Removed the remaining active Lua-version compatibility framing from docs,
  stock-test tags, and public header comments.
- Removed unused public C API compatibility backports:
  `lua_version`, `lua_isyieldable`, `luaL_newlibtable`, and `luaL_newlib`.
- Deleted pure CI alias launchers:
  `tools/ci/m5_libc_error_reentrant.sh` and
  `tools/ci/m5_tab_emptyhash.sh`; their behavior cases live in the Lua suite.
- Made recorder string ctype parsing nonblocking. If another parser owns the
  ctype mutation token, recording aborts with `CTBUSY` instead of waiting.
- Added `t-ffi-recorder-string-ctype-busy.c` to prove string `ffi.sizeof()` and
  string `ffi.new()` recorder paths abort under a busy parser token, then work
  normally after release.
- Follow-up: moved `cp_ctype_setsib()`, `cp_ctype_setname()`, rollback restore,
  and abandon paths toward local copy-then-publish so parser rollback mutates
  fewer shared `CType` slots in place.
- Follow-up: removed the `ffi.istype()` raw fallback/parser-lock path for
  non-string comparisons. It now retries the sequence-checked snapshot path and
  parks only while another parser owns the mutation token.
- Follow-up: extended `t-ffi-istype-snapshot.c` so an active parser-token test
  proves `ffi.istype()` returns the normal boolean result without taking and
  releasing the parser token itself.
- Follow-up: removed the parser-lock fallback from enum string constant
  conversion in `lj_carith.c` and `lj_cconv.c`. Enum hit and miss results now
  use sequence-checked snapshot wait/retry, and the wait helper refetches by
  `CTypeID` after native waits so retired ctype tables cannot leave stale root
  pointers behind.
- Follow-up: extended `t-ffi-enum-snapshot.c` with active-token enum hit and
  miss coverage for direct conversion, equality, and arithmetic error paths.

## Remaining locks outside `ffi.cdef`

Good reasons to keep for now:

- Interpreter FFI parser fallback waits in `lib_ffi.c` layout/string-parse
  paths, cdata field/element/layout readers, pointer arithmetic size readers,
  and `ffi.C` namespace lookup still preserve normal FFI semantics during
  parser rollback and abandoned-entry windows. `ffi.istype()` and enum string
  conversion are no longer in this bucket for stable non-string/non-cdef
  readers.
- Per-state owner claims: prevent concurrent mutation of one `lua_State`.
- Threading mutex/channel/join waits: user-visible synchronization semantics.
- Safepoint leadership and GC2 worker lifecycle waits: shutdown and collector
  safety.
- GDBJIT publication lock: debugger-facing metadata, low runtime priority.

Worth making more lockless:

- More FFI recorder-only ctype readers, because trace aborts are not visible
  language failures.
- Interpreter FFI readers only when a snapshot path can preserve normal success
  behavior or a documented busy-result contract exists.
- Remaining source guards with equivalent runtime or generated-code behavior
  coverage.

Not worth forcing right now:

- Mutable `ffi.cdef` serialization.
- Public synchronization APIs.
- GC2/legacy collector bridge points until the collector no longer depends on
  them for liveness, finalization, and sweep safety.

## Verification

Passed:

- `make -C src -j`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback m7_ffi`
- `tools/ci/lua_test.sh m7_ffi_carith_l m7_ffi_ctype_tab_retire m7_ffi_cparse_rollback`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/lua_test.sh m5_libc_error_reentrant m5_tab_emptyhash`
- `tools/ci/lua_test.sh run_stock_tests -- src/luajit --quiet lib/contents.lua`
- `tools/ci/lua_test.sh run_stock_tests -- src/luajit --quiet lib/base`
- `tools/ci/lua_test.sh run_stock_tests -- src/luajit --quiet lang/meta`
- `git diff --check`

One parallel test attempt failed because a concurrent clean build removed
`src/luajit` while another stock-test process was starting. The affected stock
target passed when rerun without overlapping a clean build.
