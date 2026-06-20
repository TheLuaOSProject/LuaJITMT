2026-06-20

Slice: published trace REF_BASE reads during trace flush dependency handling.

Changes:
- trace_flushside() now acquire-loads the REF_BASE IR cell before reading the
  parent trace number and exit number.
- trace_scope_flush_dependency() now acquire-loads REF_BASE before checking a
  side trace's parent dependency.

Validation:
- make -C src -j$(getconf _NPROCESSORS_ONLN)
- tools/ci/m6_jit_flush_hs.sh
