## FFI Enum String Snapshot

Stable enum string constant reads no longer take the cparser token on the
normal path. `lj_ctype_enumconst_snapshot()` sequence-checks the root enum's
sibling chain, copies the constant value and child type ID, and falls back to
the parser lock if a parser is active or overlaps the read.

The snapshot helper is wired into interpreted enum conversion and cdata
arithmetic, plus the recorder conversion and arithmetic readers. The old
locked lookup remains as the rollback-safe fallback path.

Coverage:
- `tests/t-ffi-enum-snapshot.c` checks interpreted enum string casts and JIT
  enum cdata/string arithmetic do not advance the cparser sequence.
- `tests/t-ffi-cparse-rollback-reader.lua` continues to cover rollback
  fallback behavior for enum string casts.
