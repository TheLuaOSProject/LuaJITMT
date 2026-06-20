FFI cdata field snapshot slice
==============================

- Added sequence-checked struct field snapshots for stable string-key cdata
  reads/writes. The helper snapshots sibling chains, anonymous subtype fields,
  field offsets, and subtype qualifiers without taking the parser token.
- Added a sequence-checked pointer-to-struct helper so cdata `->` auto-deref can
  stay off the parser token unless a concurrent parse overlaps.
- `lj_cdata_index_l()` now accepts a caller-owned `CType` snapshot, uses the
  snapshot helper for string-key struct fields and constructor constants, and
  retries under the parser token only on overlap/fallback.
- `recff_cdata_index()` uses the same field and pointer snapshots for recorder
  specialization, with the old locked lookup kept as the fallback path.
- Coverage: `t-ffi-field-snapshot.c`, rollback reader, cdata get/set, and full
  `m7_ffi`.
