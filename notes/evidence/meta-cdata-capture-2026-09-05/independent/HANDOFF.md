# Final handoff: cdata method capture v2

No concrete correctness blocker found. The durable source candidate and exact registration flags pass all 13 modes with assert and ASan/LSan. The unchanged ASan CLI also passes root’s exact Lua semantic script in interpreter and JIT modes, with leak detection enabled. All runtime/test sources remain under /tmp except the earlier explicitly requested FFI diagnosis note/evidence package.

- Apply `durable-fixture.patch`, or copy `t-meta-cdata-capture.c` to tests/ and merge `m5-registration.lua` beside the two existing Lua cases. The source is byte-identical to probe.c; no include/path rewrites are needed.
- Canonical case: `m5_meta_cdata_capture_protocol`, Linux x64. One fixture compilation, 13 fresh processes, 15 seconds each, default runtime build restored afterward.
- Exact command records: `adapted-assert-build.json`, `adapted-asan-lsan-build.json` and their per-mode/result JSON files. `registration-spec.json` gives the canonical compiler flags and wrapper list.
- Runtime source/patch provenance: `source-manifest.json`. Correctness and failure-route reasoning: `review.md`. Full schedules, limits and qualified prior-code negative control: `README.md`.
- The queued-grow failure hook returns before allocation; actual OS/allocator OOM was not induced. The original custom-allocator route and broad table-key traversal fallback were not newly certified.

## Exact wrappers

- `lj_gc2_tv_lease_acquire`
- `lj_gc2_lease_release`
- `lj_tab_wait_l`
- `lj_gc2_smr_read_enter`
- `lj_vm_call`
- `lj_vm_pcall`
- `lj_vm_cpcall`
- `lj_vm_resume`

## Modes

`basic`, `alias-source`, `alias-key`, `same-source-key`, `set-alias`, `retry-source`, `retry-key`, `retry-mt`, `retry-method`, `replace`, `growth`, `fail-growth`, `throw`.

## Compiler and runtime flags

Runtime XCFLAGS:

```text
-DLJ_FUNC_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLUA_USE_ASSERT
```

Canonical C flags:

```text
-D_GNU_SOURCE -std=gnu11 -O2 -g -Wall -Wextra -Werror -mcx16 -DLJ_FUNC_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLUA_USE_ASSERT
```

Libraries: `-lm -ldl -pthread`; each named wrapper uses `-Wl,--wrap=SYMBOL`. ASan adds `-fsanitize=address -fno-omit-frame-pointer` and uses `-O1`; target runtime instrumentation uses only TARGET_CFLAGS/TARGET_LDFLAGS. Final sanitizer runs use `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`. Earlier leak-disabled runs and the Clang C99 compile rejection remain separately identified evidence.

## SHA256

| Artifact | SHA256 |
| --- | --- |
| fixture | `1fe666138fc253fe5a70d0e6a8ebe98a454d40bb1c0a27e1029037d9b68e46d8` |
| patch | `70c8a69837a80e956bbae9a1f9d528c419a43abebbb2832d235626db586964ac` |
| assert_archive | `b2227be79b96cf6f832aec53e93e45859b8f07726fd554f97bac2ad507231ce5` |
| asan_archive | `a9797ff8c0124c8c4f7ffea72dfc2d198819bf32bbfe34a986bff423965336ff` |
| asan_cli | `227847555d2a7bb00f1b4ba1a8bb5243ae8fdc43b6b8874f5e0fd8a69003239f` |
| original_assert_probe | `736695c3b0e29afaa94e3493d0de0639cad036b7b5a2c29c52b64e5578e90542` |
| original_asan_probe | `20acbe66596fcfe440c5fd993a312b88b05e3d94f61b06d5d492e6458494a84b` |
| durable_assert_probe | `544fcf59223acaa41f82028c3e40e4d797dd5b0c5e8ad246078f3409d8f6049b` |
| durable_asan_lsan_probe | `1e5dde227dfc6d186c13421c45c3ef8ab4fb50e385a77842150ceb89b98fbb73` |
| root_lua_script | `0774a146476acbf4887481f18c8b82a4b5674d7ab0592dd1105a75cc5e41eb14` |
| older_negative_archive | `9695d469b23d70e5ff4740a635a66c7997445d1703afff0e58e78b42e6a36589` |
| older_negative_probe | `46484160933ec048bf5974392ff892e6f061cdfbc313ac972595eb787421723d` |

The implementation source in frozen normal, frozen assert, shared checkout and the isolated ASan copy is `c355b30c7978b31b499b8fe41fff1c03e8ff00f4a2a8293e1e4c74ee3823161e`. The prior-code negative archive lacks the later tag guard/FNEW repair and is not a one-diff control. No further runtime rebuild or broad test run is needed for this fixture handoff.
