2026-06-20

- Tightened the M5 empty-hash/nilnode fixture so its nilnode link assertion
  reads `Node.next` through `lj_tab_nextnode_acq()` instead of the raw
  `nextnode()` macro.
- Extended `tools/ci/m5_tab_emptyhash.sh` with a static test-tree guard that
  rejects raw `nextnode()` traversal in C fixtures. Production table walkers
  already route through `lj_tab_nextnode_acq()`; this prevents future tests
  from normalizing direct shared-link reads.
- Validation: `tools/ci/m5_tab_emptyhash.sh`.
