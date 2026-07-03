2026-06-20

- Tightened the M5 empty-hash/nilnode fixture so its nilnode link assertion
  reads `Node.next` through `lj_tab_nextnode_acq()` instead of the raw
  `nextnode()` macro.
- Historical state: `tools/ci/m5_tab_emptyhash.sh` once included a static
  test-tree guard for raw `nextnode()` traversal in C fixtures. That
  implementation-text assertion has been removed; production and fixture walkers should
  still use `lj_tab_nextnode_acq()` because `Node.next` is a shared publication
  edge.
- Validation: `tools/ci/lua_test.sh m5_tab_emptyhash`.
