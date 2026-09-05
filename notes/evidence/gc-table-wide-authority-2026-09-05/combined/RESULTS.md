# Final combined Linux validation

The requested combination passes the normal, helper/assertion, and target-only
ASan plus leak-detection matrices. There were no failed builds, failed fixture
executions, timeouts, or sanitizer reports in this run. No source or test edits
were made after the combination was created, and no shared files were changed.

The exact base is `4e7a270e7dd8b79ec4ad5611cd7bbd6226783b0d`, followed by:

- `source-and-tests.patch`, SHA-256
  `2a7f49960f70123898715bd6dc5445cdd776f54d62917a8d3e3890b21443fa49`.
- `pre-mt-cdata-guards-review.patch`, SHA-256 recorded in `apply-results.json`.

Both patches applied cleanly. `source-manifest.json` records all 792 tracked
source/test/build inputs across the three variants. The landed M5 cdata Lua and
C protocol registrations remain byte-identical to the base. Only five production
files differ from the base: `lj_arena.c/.h`, `lj_gc2.c/.h`, and `lj_record.c`.
The cdata capture implementation in `lj_meta.c` is exactly the landed base.

After testing, a read-only comparison at shared HEAD `30cf1d99` confirmed that
all 210 tracked shared `src/` files, including the pending wide changes, match
this validated combination byte-for-byte. That comparison is preserved in
`shared-source-comparison.json`; it does not authorize a changed later source
state. The core hashes are:

| Source | SHA-256 |
| --- | --- |
| `src/lj_arena.c` | `b90b713ca6183ec36ca594c16c37d110e87fcebd815aa3f207da3d3f4be83973` |
| `src/lj_arena.h` | `c1e19e04c5a65aa05f7f37a195864995988610f06b4fb9ec897405982a4cd1c8` |
| `src/lj_gc2.c` | `5557bc243f94f5d310287494cc3560d91931455eaa8338ba480f84a33edf45a1` |
| `src/lj_gc2.h` | `244b10c49ad036b661a08adc178e6b8247d8ff1b18ed63bc035d3440df16a9f9` |
| `src/lj_record.c` | `e5d872f8fc9af3fe10643f6a328084d0d582c3a9753bb70bcccced1a0e65ef6e` |
| `src/lj_meta.c` | `c355b30c7978b31b499b8fe41fff1c03e8ff00f4a2a8293e1e4c74ee3823161e` |

## Build and sanitizer qualification

All builds are fresh Linux x86-64 static builds with debug information. Normal
has no test-helper or runtime-assert macros. Strict and ASan enable FUNC, GC2,
TAB, ARENA, TRACE helpers and `LUA_USE_ASSERT`. Exact build commands and outputs
are in `build-{normal,strict,asan}.json/.stdout/.stderr`; binary hashes are in
`build-binary-manifest.json`.

ASan uses Clang with only
`TARGET_CFLAGS=-O1 -fsanitize=address -fno-omit-frame-pointer` and
`TARGET_LDFLAGS=-fsanitize=address`. Host generators are not instrumented.
`asan-instrumentation.json` checks the absence of sanitizer references in
minilua/buildvm host objects and their presence in the GC2, meta, and recorder
target objects. No leak-disable setting was used at any stage. Every ASan
runtime process has `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`; there are no
runtime suppressions.

All build/functional work ran on CPUs 0-15. Other agents were active, so this is
not a system-isolation or performance study. No timing benchmarks were rerun.
Per-command durations serve only as bounded-execution evidence.

## Passing matrix

Each of normal, strict, and ASan passes 387 stock cases with JIT off and 509
with JIT on. Each also passes, in fresh processes:

- Landed cdata capture Lua with JIT off/on, including small, aligned and Huge
  cdata, callback/collection reentry, caught errors, aliasing and later hops.
- All eight recorder method-guard modes with JIT off/on. JIT mode retains the
  fixture's native-exit witnesses before and after method mutation, vector
  movement, method lifetime changes, or base-metatable replacement.
- Weak modes, weak-metatable bridge, finalizer peer collection, finalizer
  spawning and FFI FINREG Lua with JIT off/on.
- The canonical concurrent table resize selection with default counts: weak,
  gcmark, gckey, weakkey, weakmeta, finalizer, metatable, len, traversal,
  nextchurn, nextinvalid, tableclear, tablelib, tablelibshift, and metadispatch,
  in both modes. Defaults remain 3 mutators, 768 main repetitions, 192 traversal
  and finalizer/key objects, and 2 GC workers; no stress assertion was weakened.
- The JIT table selection `jitstore,jitread,jititer,weakfinjit` with its existing
  2,200 store/read repetitions and native execution observers.
- The exact canonical Lua bodies for first MT activation/retired-slot reuse
  and first GC-worker activation, with native trace/flush assertions retained.

Strict and ASan additionally pass 25 C-fixture executions each:

- Full coalescing/overflow, traversal, TNEW and truthful FNEW, including their
  persistent W, paused scanner/publisher, full namespace, emitted reuse,
  exact-token, and protected-memory controls.
- Huge-tail boundary/payload, logical bounds, private realloc and published
  traversable refusal, readers, map/insertion failure, transfer/fini controls.
- All 13 landed cdata C protocol modes: basic, alias-source, alias-key,
  same-source-key, set-alias, retry-source, retry-key, retry-mt, retry-method,
  replace, growth, fail-growth, and throw. The canonical eight linker wrappers
  and exact helper/assert flags are preserved in the command records.
- Recovery, table-store guard, cdata and userdata FINREG roots, FFI weak
  newindex, close finalizers, and the paused-reclaimer root-ABORT fixture.

`strict-fixtures.json` and `asan-fixtures.json` each contain 13 successful
compilations and 25 executions. `{normal,strict,asan}-lua.json` each contains
21 executions. Including three runtime builds, the complete run records 142
successful bounded commands and 113 test-process executions. All source hashes
were rechecked after the runs. The cdata queue failure control injects the
documented pre-allocation failure; it does not claim actual allocator OOM.

## Remaining limits

The shared cdata JIT hammer is known to fail at line 80 because active-MT cdata
recording retains its refusal. It was deliberately not rerun or counted as
passing in this matrix; no refusal or assertion was weakened. Its existing
failure remains part of the broader stability/performance record.

This matrix adds exact final-source ASan/LSan and component-interaction evidence.
It does not eliminate the full finite authority namespace veto, permit published
TRAVERSABLE Huge realloc, resolve general SMR/plain-arena writer dependencies,
or establish that the complete runtime is nonblocking. Windows and macOS were
outside this Linux validation scope.
