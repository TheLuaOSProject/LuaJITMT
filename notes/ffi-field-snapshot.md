FFI cdata field snapshot slice
==============================

- Added sequence-checked struct field snapshots for stable string-key cdata
  reads/writes. The helper snapshots sibling chains, anonymous subtype fields,
  field offsets, and subtype qualifiers without taking the parser token.
- Added a sequence-checked pointer-to-struct helper so cdata `->` auto-deref can
  stay off the parser token unless a concurrent parse overlaps.
- `lj_cdata_index_l()` now accepts a caller-owned `CType` snapshot and uses
  ID-rooted wait/retry helpers for string-key struct fields, constructor
  constants, and pointer `->` auto-deref. It no longer takes the ctype parser
  token for this fallback path.
- `lj_ctype_getfieldq_wait()`, `lj_ctype_ptrstruct_wait()`, and
  `lj_ctype_info_wait()` park in native time while another parser owns the
  token. They are rooted by `CTypeID`; callers refetch table `CType *` state
  after a wait and only return copied field snapshots.
- `recff_cdata_index()` uses the same field and pointer snapshots for recorder
  specialization, with busy paths still aborting rather than parking.
- Coverage: `t-ffi-field-snapshot.c` now actively holds the parse token across
  direct field get/set, pointer auto-deref get/set, misses, metatype dispatch,
  and constructor constants. The rollback reader also covers failed constructor
  constants.
