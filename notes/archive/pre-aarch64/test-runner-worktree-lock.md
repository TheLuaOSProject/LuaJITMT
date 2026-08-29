# Test runner worktree lock

`tools/test.lua` now takes an atomic per-worktree lock at
`src/.lj-test-run.lock` for the duration of a test run. The trigger was a false
build-stability failure: two harness processes in the same checkout overlapped
`make clean` and `make -j`, causing `libluajit.so` to link while another runner
removed or regenerated `_dyn.o` files.

This is harness-only serialization, not a runtime lock. It keeps one checkout
coherent while still allowing parallel test work through separate git worktrees,
which is the intended workflow for subagents and release-platform checks.

The lock can be disabled with `LJ_TEST_DISABLE_RUN_LOCK=1` for explicit
debugging, and wait timeout is controlled by `LJ_TEST_RUN_LOCK_TIMEOUT`.
