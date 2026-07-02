# FFI Layout Size Helper Loads

`ffi_layout_vlsize()` now snapshots local layout `CType` copies through helper
loads while scanning struct fields for variable-length array tails, checking the
raw VLA array type, reading element size metadata, and computing the final size.

`ffi_layout_sizeof_snapshot()` now snapshots the raw type's `CType.info` /
`CType.size` values before deciding whether `ffi.sizeof()` needs an element
count or can return a direct size.

Guardrail:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh` rejects direct local `cur.info`,
  `cur.size`, `cur.sib`, `elem.info`, `elem.size`, `elem.sib`, `ct.info`,
  `ct.size`, or `ct.sib` reads in the layout size snapshot path.

Validation:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `git diff --check`
