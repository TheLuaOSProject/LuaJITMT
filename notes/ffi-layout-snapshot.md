# FFI layout query snapshots

## Context

`ffi.sizeof`, `ffi.alignof`, and `ffi.offsetof` still serialized existing
ctype/cdata layout reads through the cparser token. That was safe, but it kept
stable layout queries on the parser lock path even when no parsing was needed.

String type expressions first try the direct resolver for predefined/numeric
names, published typedef/tag names, and derived pointer/array spellings. Strings
outside that direct grammar still use the parser. `ffi.offsetof()` parses
anonymous or otherwise non-direct strings under the parser token, then uses the
same sequence-checked layout snapshot for the read-only field walk.

## Change

`lib_ffi.c` now has a sequence-checked layout snapshot reader for stable ctype
layout arguments:

- Begin by acquire-loading `CTState.parse_token`; odd means retry after parser
  publication, or abort recording with `CTBUSY`.
- Snapshot `top` and the RCU-published CType table header.
- Bounded-walk raw/ref/attribute, VLA/VLS, info/alignment, and field/subtype
  chains through acquired CType field loads.
- Recheck `parse_token`; any overlap with parser mutation retries through the
  native wait helper.

Predefined immutable type IDs now take the same acquired field snapshot without
waiting for an unrelated active parser token. This covers predefined
`ffi.sizeof(ct)`, `ffi.alignof(ct)`, `ffi.offsetof(ct, field)` misses,
`ffi.new(ct, ...)`, and the shared metadata read used by `ffi.cast(ct, value)`.
If the walk leaves the predefined range, it falls back to the existing
sequence-checked retry path.

Parser-backed strings remain the fallback for declaration forms outside the
direct resolver. Runtime active-parser windows, racing table growth, and
inconsistent chains retry through native waits; recorder paths abort with
`CTBUSY` instead of parking.

## Coverage

Added `tests/t-ffi-layout-snapshot.c`, wired into `m7_ffi_typeinfo_snapshot`.
It verifies stable `sizeof`, `alignof`, `offsetof`, bitfield `offsetof`, and
VLA `sizeof(ct, n)` do not advance the cparser sequence. It also verifies
string `ffi.offsetof("struct ...", field)` advances the parser sequence by one
lock/unlock pair only, proving the read-only field walk does not reacquire the
parser lock.

The fixture now also holds the parser token while running predefined
`sizeof`, `alignof`, `offsetof`, `new`, and `cast` operations. Those operations
complete without parking, while user-defined struct, VLA, and field-offset
queries still prove the native wait/retry path.
