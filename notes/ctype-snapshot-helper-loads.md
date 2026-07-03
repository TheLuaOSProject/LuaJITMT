# CType Snapshot Helper Loads

The ctype snapshot helpers now use helper-backed loads for local copied
`CType` records while validating abandoned records, following anonymous subtype
field chains, resolving pointer-to-struct auto-deref targets, and accumulating
stable typeinfo/layout metadata.

Converted helpers:

- `lj_ctype_snapshot()`
- `ctype_snapshot_copy()`
- `ctype_getfieldq_snapshot_rec()`
- `lj_ctype_ptrstruct_snapshot()`
- `lj_ctype_info_snapshot()`

Invariant check:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh` rejects direct local `ct.info`,
  `ct.size`, `ct.sib`, `cct.info`, `cct.size`, or `cct.sib` reads in these
  ctype snapshot helpers.

Validation:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `git diff --check`
