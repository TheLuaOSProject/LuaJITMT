# FFI layout query snapshots

## Context

`ffi.sizeof`, `ffi.alignof`, and `ffi.offsetof` still serialized existing
ctype/cdata layout reads through the cparser token. That was safe, but it kept
stable layout queries on the parser lock path even when no parsing was needed.

String type expressions still need the parser and intentionally remain on the
locked path.

## Change

`lib_ffi.c` now has a sequence-checked layout snapshot reader for non-string
ctype arguments:

- Begin by acquire-loading `CTState.parse_token`; odd means retry under lock.
- Snapshot `top` and the RCU-published CType table header.
- Bounded-walk raw/ref/attribute, VLA/VLS, info/alignment, and field/subtype
  chains through acquired CType field loads.
- Recheck `parse_token`; any overlap with parser mutation retries under the
  existing locked path.

The existing locked path remains the fallback for strings, active parser
windows, racing table growth, or inconsistent chains.

## Coverage

Added `tests/t-ffi-layout-snapshot.c`, wired into `m7_ffi_typeinfo_snapshot`.
It verifies stable `sizeof`, `alignof`, `offsetof`, bitfield `offsetof`, and
VLA `sizeof(ct, n)` do not advance the cparser sequence, while a string layout
query still does.
