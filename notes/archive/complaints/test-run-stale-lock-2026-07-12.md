# Test runner stale lock has no liveness identity (2026-07-12)

Status: resolved the same day after the user repaired the shared test harness.
The details below remain as an environment incident record.

`tools/ci/lua_test.sh` serializes shared-tree builds with
`src/.lj-test-run.lock`, but its `owner` file records only a timestamp and
command.  If a runner is killed without executing its shell trap, the directory
remains indefinitely and later healthy runners wait until the default 900-second
timeout.  The current stale instance names `m3_gc_root_pending_race`; process
inspection shows no active test/build runner holding it.

Please make the lock recoverable without guessing.  A suitable fix would record
the launcher PID plus a process-start identity (Linux `/proc/$pid/stat` start
time, or a portable nonce held by the live process), then atomically remove an
owner only when that identity is provably dead.  A plain PID alone is vulnerable
to PID reuse.  The Lua-side `with_directory_lock` path should use the same owner
format and stale-owner rule.  Explicit `LJ_TEST_DISABLE_RUN_LOCK=1` remains a
useful escape hatch for isolated snapshots, but is unsafe as automatic recovery
in a shared working tree.
