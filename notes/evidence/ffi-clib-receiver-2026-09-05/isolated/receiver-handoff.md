# Captured CLibrary receiver specialization handoff

The isolated patch `receiver-specialization.patch` changes only `src/lj_crecord.c`. Its SHA-256 is `77531ea3eb0977a53ff7413cbad718543bc8952f16b216269523e6ba57664837`; final source SHA-256 is `d9117ff3214f258cc84648725effcd498f3d1e93a774a94efd65e81ed11f2bff`.

The patch is independent of the method lookup change. The validation trees inherit ROOT's exact `lj_record.c` method-guard source SHA-256 `07116bc933781976c91453a9ca89a46aef4a81d80bf71ab2f6e5269383fcce87`, and add this one receiver guard. All 224 tracked runtime/generator inputs are byte-identical between candidate, strict, and ASan. Compared with e34282576c7df0180e8113a4cfba07fd637a36b3, only lj_record.c and lj_crecord.c differ. `runtime-input-identity.json` records every hash.

## Why the guard belongs in the builtin recorder

Ordinary CLibrary metamethod dispatch specializes the namespace in `lj_record_mm_lookup`, but Lua can capture `debug.getmetatable(lib).__index` or `.__newindex` and call it directly. `recff_clib_index` previously sampled the receiver subtype at recording time and then exported that namespace's constants or extern addresses without guarding the runtime receiver. A native loop warmed on namespace A therefore accepted namespace B or an IO userdata, while continuing to read or write A.

The new guard specializes `J->base[0]` against an IRT_UDATA KGC constant. It executes before the recorder emits the namespace-specific result or extern memory operation, and applies to both `rd->data` modes. Identity implies the sampled specialized subtype for this retained object; `lj_udata_specialize` publishes that subtype once. A typed KGC constant also retains the exact namespace through the existing current-trace and installed-trace GC roots, preventing identity from becoming a recycled address. A KPTR-only comparison would not provide that retention.

No runtime helper, allocation, lock, or new wait is added. The existing guards and shared-MT refusal elsewhere remain unchanged. This patch does not claim that direct CLibrary recording is generally safe under every shared-MT cache/lifecycle mutation; those are separate known obligations.

The equality guard precedes the generated read/store. Existing snapshots resume the failed builtin operation after already-completed Lua effects. The tests below explicitly check the preceding store executes once, including an error from an installed side trace. The generic KGC scanner marks the namespace, and userdata traversal follows its metatable/environment/cache roots. No optimizer exception or method immutability assumption is introduced.

## Final fixture and validation

- `captured-clib-receiver.lua`: SHA-256 `aa0d4db920b877a17fdb2c83063ac495569ea6790fe09aafa2363ddd954e87ab`.
- `namespace-symbol-lib.c`: SHA-256 `8ce51fb90a6c101f208886b5b0dd27a4f9bb19c33bcbde696afb3b101e1d38aa`.
- Build two ordinary shared libraries from that C file with `-shared -fPIC -O2 -DNAMESPACE_VALUE=11` and `=29`.
- Run each case in a fresh process as `luajit -jon captured-clib-receiver.lua CASE LIB11 LIB29`, also with `-joff` for interpreter controls. The recorded driver applies a 30-second bound; all final runs finished in well under a second each.
- Ten cases: index-other, index-type, newindex-other, newindex-type, index-life, newindex-life, index-side-other, index-side-type, newindex-side-other, newindex-side-type.

Each native positive warmup reports two root exits and an actually executing side linked to that root. Root receiver changes produce a real old-root exit. Side-only receiver changes execute an already-installed side and produce one or two old-side exits; the unchanged root remains valid. Distinct namespaces expose different extern values, so both loads and stores have observable targets. Root errors have exactly one preceding store, side errors exactly 40, and successful runs exactly 80. Namespace A remains unchanged where required. Lifetime cases drop the only Lua reference, perform a full collection, and retain the namespace only when JIT traces retain the KGC receiver.

All 60 final positive runtime processes pass: ten cases times interpreter/JIT times normal/assert/ASan. Strict and ASan use ROOT's GC2/FUNC/TAB/ARENA/TRACE/XSAVE helpers and assertions. ASan uses Clang -O1 target instrumentation with `detect_leaks=1:abort_on_error=1`; `asan-instrumentation.json` confirms uninstrumented hosts and instrumented lj_crecord.o. The fixture libraries are ordinary uninstrumented C builds.

The exact baseline and the exact method-guard-only candidate each run all 20 final controls: ten interpreter successes and ten expected native semantic/retention failures. This isolates the receiver fix from ROOT's first method-guard fix. There are 100 final runtime processes in total, with 80 successes and 20 deliberately failing native controls; ten shared-library compile commands and three receiver runtime rebuilds also succeed. Complete commands, cwd, environment, exact source/fixture/library/binary hashes, stdout/stderr, and terminal exits are in the per-variant receiver-results and receiver-build JSON files.

The initial six-case fixture and its baseline/fixed results are retained in `initial-receiver/`. The first side extension incorrectly required the final-iteration side (trace 3) to receive every error: an error at the branch boundary correctly exited the other installed side (trace 2). That test-only overconstraint, two failed positive assertion outputs, and the diagnostic trace log are preserved in `side-first-fixture/` and `receiver-side-diagnostic*`. The final fixture enumerates all already-installed root-linked sides and requires a real exit from that frozen set. No runtime source changed during this fixture correction.

## Separate baseline findings

`captured-clib-index-type.lua` and its six-run result established the initial direct-call bug on ROOT baseline/method-only/strict/ASan inputs. `clib-cache-lifecycle.lua` and `additional-results.json` preserve two other baseline bugs: native lookup ignores later debug-environment overrides, and native lookup ignores an explicit semantic close through exposed builtin __gc. Those still require runtime environment/lifecycle authority and are not claimed fixed here. Merely retaining the namespace does not establish it remains semantically open.

No shared workspace source or tests were edited. ROOT can apply this source patch separately, copy/rename the two final fixture sources, and register the ten cases in the appropriate Linux test suite. ROOT owns final shared-tree canonical/stock validation and release decisions.
