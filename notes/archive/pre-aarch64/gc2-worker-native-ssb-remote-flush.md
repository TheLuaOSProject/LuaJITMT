# GC2 worker native SSB remote flush

`t-gc-workers.lua` worker-count churn exposed two default-allocator failure
modes while `collectgarbage("collect")` closed a mark fixpoint with parked GC2
workers enabled:

- a timeout with the leader repeatedly handshaking from `lj_gc2_fixpoint_round()`
  while a worker drained published SSB nodes;
- intermittent allocator aborts such as `double free or corruption`.

The line-level hang stack showed the leader remote-acking native TGs with
`LJ_GC2_HS_FLUSH_SSB` while a no-Lua-stack GC worker TG was active in
`gc2_worker_main()`. GC workers spend their whole lifetime in native state, but
they also run GC traversal under their own TG. Remotely swapping that worker
TG's active SSB can race traversal-local rescan publication.

`lj_safepoint_apply_tg()` now skips remote SSB flushes for no-Lua-stack peer TGs.
Native mutators still publish their active SSB because they carry `cur_L`; the
current leader still flushes its own TG. GC workers publish/drain their own SSB
at the existing GC-owner boundaries.

Verification:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- 150 iterations of `MALLOC_CHECK_=3 src/luajit -joff tests/t-gc-workers.lua`
- 80 iterations of `LJ_GC_WORKERS_CHURN_ROUNDS=4 MALLOC_CHECK_=3 src/luajit -joff tests/t-gc-workers.lua`
- `tools/ci/lua_test.sh m3_gc2_worker_scheduler`

Follow-up observed while broadening verification:

- `tests/t-weak-modes.lua -joff` currently fails at line 120 with
  `weak-kv table kept a one-cycle hash entry`. The same failure reproduced after
  backing out the production GC2 edit, so it is tracked separately from this
  worker SSB remote-flush race.
