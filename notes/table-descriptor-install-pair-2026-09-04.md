# Atomic descriptor installation

Date: 2026-09-04. Base: `a649f737d9841e1bf17f9102fb526d6bfb6c29e3`.

`lj_tab_resize_desc_install()` now publishes table control and the cold array
capacity shadow with one `cmpxchg16b`. The old separate shadow store could
overwrite a competing descriptor's capacity even when the delayed installer's
control CAS subsequently failed. The paired CAS preserves the sampled weak
cycle/state; a concurrent semantic weak update rejects the single attempt
without overwriting either current word.

Only the caller that claimed PREPARED -> INSTALLING has initial publication
authority. The existing cancellation and registry lifetime protocol therefore
handles a failed pair CAS without allowing another helper to reinstall the
same descriptor. This fixes the metadata substrate; production table resize
still uses its existing structural owner and migration path.

## Regression evidence

`exercise_install_competing_generation()` pauses installer A before
publication, performs a real array growth, and installs descriptor B. It checks
that A's cancellation preserves B's capacity and semantic weak state, that B
can finish, that VM guards balance, and that the original Lua value survives.

The test failed against the separate-store implementation at
`assert(lj_tab_acap_acq(t) == acap + 8u)`. For this reproduction, the existing
BEFORE_CONTROL_CAS hook was moved before the old shadow store to expose the
actual preemption window. No production behavior was otherwise changed. The
paired implementation passes the same schedule; its hook precedes the entire
atomic publication.

`exercise_install_weak_update()` separately changes semantic weak state at
that boundary. The losing attempt must terminalize without a guard leak, and
a fresh attempt must succeed while preserving the newer weak state.

## Linux validation

The baseline shared suite passed:

```sh
tools/ci/lua_test.sh m5_tab_resize_descriptor m5_tab_resize_copy_helper m5_tab_struct_owner
```

For isolated post-fix builds, `git archive` of the base commit was extracted
to `/tmp/lj-table-review-linux-e47v_3n5`, then only `src/lj_tab.c` and
`tests/t-tab-resize-descriptor.c` were overlaid. Concurrent GC edits and shared
build products were excluded. The full descriptor fixture passed each build:

| Build | Result |
| --- | --- |
| GCC, assertions and both table/GC helper sets, `-Werror` | PASS |
| Clang, assertions and both helper sets, `-Werror` | PASS |
| Clang AddressSanitizer, same assertions/helpers, leak detection enabled | PASS |

Each compiler build followed `make -C "$review_dir/src" clean`. Runtime flags:

```sh
taskset -c 0-15 make -C "$review_dir/src" -j6 BUILDMODE=static CC=clang \
  XCFLAGS='-DLUA_USE_ASSERT -DLJ_TAB_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -Werror'
```

Use `CC=gcc` for the GCC run. The ASan run additionally set
`TARGET_CFLAGS='-fsanitize=address -fno-omit-frame-pointer'` and
`TARGET_LDFLAGS='-fsanitize=address'`. The fixture linked the corresponding
static library with `-std=gnu11 -O2 -Wall -Wextra -Werror -mcx16`, the same
preprocessor definitions, and `-lm -ldl -pthread`; the ASan fixture also used
the sanitizer flags. Its runtime command was:

```sh
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 timeout 45s "$review_dir/descriptor-asan"
```

All three printed:

```text
t-tab-resize-descriptor OK: persistent ids survive GC and enter SMR retirement
```

Build logs: `/tmp/lj-table-review-gcc.log`,
`/tmp/lj-table-review-clang.log`, `/tmp/lj-table-review-asan.log`.
Initial ASan build attempts incorrectly instrumented the host generators:
first their sanitizer runtime was absent at link, then LeakSanitizer reported
generator allocations retained until exit. The final successful build applies
ASan to target objects and the fixture, leaving host generators uninstrumented.
This is C memory-safety evidence for the fixture, not instrumentation of JIT
machine code or proof of all concurrent schedules.

Platform validation already underway when Linux-first sequencing was
requested is recorded separately in
`notes/table-descriptor-platform-validation-2026-09-04.md`. Further Windows and
macOS work is deferred until preparation for the next release.
