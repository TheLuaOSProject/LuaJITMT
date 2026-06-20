# CType.next atomic helper audit

## 2026-06-20

- Added `ctype_next_acq()` / `ctype_next_rel()` for the 16-bit `CType.next`
  hash-chain link.
- Routed shared ctype hash walkers, snapshot readers, FFI snapshot helpers, and
  focused ctype fixtures through the helper.
- Extended `tools/ci/m7_ffi_ctype_hash_publish.sh` with a focused grep guard
  over the ctype/FFI files and fixtures that should not spell direct
  `ct->next`/`dst->next`/`basect->next` hash-chain access.

## Deliberate exclusions

- `src/lj_cparse.c` has parser-local `next` fields on rollback/allocation and
  declaration-stack structs. Those are not `CType.next`.
- `src/lj_cparse.c` comments mention preserving `ct->next` while abandoning
  ctypes; the focused guard does not match those comments.
- Whole-`CType` struct copies remain part of the existing ctype publication and
  parser rollback machinery. This slice only centralizes explicit hash-chain
  link loads/stores and guards against direct field regressions in the shared
  ctype hash publication paths.
