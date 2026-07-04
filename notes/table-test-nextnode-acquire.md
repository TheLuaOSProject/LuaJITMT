2026-06-20

- Tightened the M5 empty-hash/nilnode fixture so its nilnode link assertion
  reads `Node.next` through `lj_tab_nextnode_acq()` instead of the raw
  `nextnode()` macro.
- Production and fixture walkers should use `lj_tab_nextnode_acq()` because
  `Node.next` is a shared publication edge; keep that reason beside the helper
  and cover failures through table behavior fixtures.
- Validation: `tools/ci/lua_test.sh m5_tab_emptyhash`.
