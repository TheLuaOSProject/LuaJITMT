# CType.next atomic helper audit

## 2026-06-20

- Added `ctype_next_acq()` / `ctype_next_rel()` for the 16-bit `CType.next`
  hash-chain link.
- Routed shared ctype hash walkers, snapshot readers, FFI snapshot helpers, and
  focused ctype fixtures through the helper.
- Documented the rule that shared `CType.next` hash-chain links must use the
  acquire/release helper. Ctype publication, lookup, snapshot, and rollback
  fixtures cover the behavior; the helper comment carries the implementation
  invariant.

## Deliberate exclusions

- `src/lj_cparse.c` has parser-local `next` fields on rollback/allocation and
  declaration-stack structs. Those are not `CType.next`.
- `src/lj_cparse.c` comments mention preserving `ct->next` while abandoning
  ctypes; those parser-local rollback notes are outside the shared hash-chain
  publication invariant.
- Whole-`CType` struct copies remain part of the existing ctype publication and
  parser rollback machinery. This slice only centralizes explicit hash-chain
  link loads/stores; comments beside the helper layer document that shared
  ctype hash publication paths should not use raw link access.
