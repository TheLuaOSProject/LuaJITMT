# GC2 Weak Overflow Node Validation

GC2 weak overflow nodes are raw side-list records used when the bounded weak
snapshot vector is absent or full. Clear, strong-frontier close, and reset/free
paths walked that list and read `tab` / `next` without first proving the node
record was still registered memory.

This slice mirrors the other raw side-list checks:

- guard `gc2_weak_overflow_free()` before reading an overflow node link;
- guard `gc2_weak_clear_overflow()` before reading the table payload or next
  link;
- guard `gc2_weak_trace_close_frontier()` the same way.

Payload table validation remains handled by the weak-table candidate helper; the
new guard is specifically for the overflow node record itself.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m8_weak`
- `tools/ci/lua_test.sh m3_gc2_paranoia`
- `tools/ci/lua_test.sh m0_matrix`
