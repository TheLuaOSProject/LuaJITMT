# Permanent pure-cdata optimizer regressions

The tests-only candidate is frozen in `tests-only.patch`. It changes six new fixtures and the existing M6 registry, without any production source edits. Both new canonical entries and all 19 direct assertion cases passed.

## Source and integration boundary

- Base: exact `8d342cd6456d2f93ba07a779cdde30d4806eb90f`, including the landed mode-0 TG poll guard.
- Required implementation: copied, unchanged `implementation.patch`, SHA256 `6eff86feefc6299c35fa74d3b03562fdf4476ffbbae2e9c7a6be5f9d7a537853`. This is finreg's final `handoff/candidate-with-comment.patch`; only `src/lj_jit.h`, `src/lj_opt_loop.c`, and `src/lj_opt_mem.c` differ from the base.
- Tests patch SHA256: `09bbd814ff7e5f620fa16a2460632d727781d787f6bde62abaf69dd9973b4124`.
- `final-source-manifest.json` hashes all 224 tracked runtime/generator inputs in both runtime trees, every final test source, both runtimes/archives and all four C fixture executables. Both runtime trees have identical production inputs. Reconstructing the three changed files from the base plus the exact implementation patch matched byte for byte (`source-reconstruction.json`).
- `tests-apply-check.json` records clean `git apply --check --whitespace=error-all` against the shared base without modifying it. The source patch remains a separate integration prerequisite; a tests-only apply check does not validate that prerequisite.
- This package does not include the separate CALLXS callback geometry correction. Root's combined poll/callback/optimizer build and validation remain the integration gate.

## Durable coverage

| Fixture | Required behavior |
| --- | --- |
| `tests/t-jit-cdata-pure.lua` | Actual native TEXIT, self-loop trace, mode-0 XPOLL, exact pre-loop root/node/method identity guards, and no repeated copied method chain. Seven fresh modes challenge in-place `__index`, `__newindex`, missing/non-function methods, resize, old-method lifetime, and root replacement with exact values/call counts/errors. |
| `tests/t-jit-cdata-pure-side.lua` | Actual side-to-root link and side TEXIT, followed by in-place method mutation while that side remains published; exact updated behavior and a further side exit. |
| `tests/t-jit-cdata-pure-exclusions.lua` | Otherwise mode-0 native roots retain the repeated method chain for allocation, Lua object store, newref, table.clear, foreign CALLXS, indirect XSTORE and libm CALLN. Relevant excluded IR operations and numerical/state results are required. |
| `tests/t-jit-cdata-pure-profile.lua` | Profiler activation flushes the old root, the profiled root uses mode-1 polling and retains its repeated chain, and a real sample mutates the method with exact results computed from observed calls. |
| `tests/t-jit-cdata-pure-phase.c` | Separate `gate` and `worker` processes first prove the hoisted mode-0 native root. A peer observes actual native `jit_base` during MARK, then closes the gate or starts a real global GC worker. The owner exits early, mutates the method only after exit, and performs the exact remaining calls. Worker pool teardown, subsequent traced behavior and final IDLE are required. |
| `tests/t-jit-cdata-pure-error.c` | GNU ld wrapper injects one real `lj_err_mem` during the private unroll scope. The protected return must be exactly `LUA_ERRMEM`, the private flag must clear, and a fresh correct trace must subsequently produce a TEXIT. |

`tests/suites/m6_jit.lua` registers `m6_jit_cdata_pure` and `m6_jit_cdata_pure_lifecycle`, including them in the M6 case list. Every child process has a 20-second bound. The phase peer has a five-second observation bound and the native loop is finite (2,000,000 iterations); actual early exit, rather than elapsed time, is the progress witness.

The suite entries are explicitly Linux/x64. The lifecycle error fixture requires GNU ld `--wrap=lj_mem_realloc`; Windows/macOS support is deferred with the user scope. No production test-helper macro is needed. The C fixtures use `-lm -ldl -pthread`; only the error fixture additionally uses `-Wl,--wrap=lj_mem_realloc`. Strict direct compiles use `-std=gnu11 -O2 -g -Wall -Wextra -Werror -mcx16 -DLUA_USE_ASSERT` and the assertion archive. Exact commands are retained in the result JSON and canonical logs.

## Adaptation and provenance

`raw/` preserves the final raw fixtures/drivers copied from `/tmp/lj-premt-cdata-hoist-20260905-oa96m15y`; their original hashes are in `setup-manifest.json`.

- Semantic, side and profiler Lua fixtures are copied byte-identically from `t-premt-cdata-pure.lua`, `side-link.lua` and `profile-negative.lua`.
- Exclusions retain the seven supported modes, remove the obsolete direct-callback eligibility mode and unused callback argument, and strengthen mode-0 XPOLL plus allocation/libm result assertions. Authentic generated callbacks are covered by the separate callback geometry regression; this package does not revive the invalid direct-callback optimizer control.
- Phase fixture combines the final `phase-gate.c` and `global-worker.c` schedules into separate fresh-process modes, adding an explicit pre-MARK proof that the root is hoisted with mode-0 XPOLL. Method mutation remains on the owner after native exit.
- Error fixture derives from `flag-error.c`, strengthens nonzero status to exact `LUA_ERRMEM`, and adds an actual TEXIT witness after recovery.
- C formatting changed before final compilation. The `strict/` tree's fixture copies predate that formatting, but `validate-strict.py` explicitly compiles/runs the final files in `tree/tests`. The final fixture hashes and strict commands therefore refer to the same final source bytes.

## Validation

- Initial normal static and assertion-only static builds passed using CPUs 0–15, `CCDEBUG=-g`, `TARGET_STRIP=:`; assertion runtime adds `XCFLAGS=-DLUA_USE_ASSERT` (`build-results.json`).
- Both canonical entries passed (`canonical-results.json` plus complete stdout/stderr). Canonical `build_default` rebuilt the normal tree in default mixed mode. The initial static hashes remain in `build-results.json`; final canonical binary/archive hashes are explicitly recorded in `final-source-manifest.json`.
- All 19 direct assertion cases passed (`strict-results.json`), covering seven semantic mutations, side reentry, seven exclusions, profiler, gate, worker and OOM.
- Normal gate/worker exited at iterations 56,406 / 54,575; assertion gate/worker exited at 131,988 / 237,488, below the 2,000,000 bound. Exact continuation values and call counts passed in every case. These are schedule witnesses, not timings or performance measurements.
- Both OOM variants returned status 4 (`LUA_ERRMEM`) with one injection and the unroll flag zero, then executed a fresh correct native trace.

No new runtime failure occurred in this packaging run. Original optimizer controls, including prior failed trials and baseline negatives, remain in the owner's original evidence package. This bounded task did not repeat ASan, stock suites, platform testing or performance measurements. It establishes durable targeted regression coverage for the specified source boundary; it does not establish general runtime nonblocking behavior or validate the later combined callback source.
