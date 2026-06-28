# FFI cdata tostring snapshots

`ffi_meta___tostring()` now reads cdata type metadata through the shared
`ffi_ctype_info_read()` helper instead of directly indexing the shared ctype
table. The root cdata type is copied into a local `CType` before deciding
whether the value is a ref, complex number, 64-bit integer, function, enum,
pointer, struct, or vector.

Reference targets and pointer child targets are snapshot separately because
those branches need metadata from a second ctype record. This preserves the
existing formatting and metatype dispatch behavior while avoiding unlocked
shared `CType.info`/`CType.size` reads during concurrent parser activity.
The helper keeps the same wait behavior for ordinary interpreter calls and
centralizes the active-recorder `CTBUSY` abort policy for FFI API type reads.

Coverage:
- `tests/t-ffi-tostring-snapshot.c` checks int64, uint64, enum, struct
  metatype, and pointer-to-metatype `tostring()` behavior without advancing
  `CTState.parse_token`.
- The same fixture holds the parser token and verifies cdata `tostring()`
  waits from a native region until a consistent ctype snapshot is available.
- The fixture is wired into `m7_ffi_typeinfo_snapshot`; this is behavior
  coverage rather than source-search coverage.
