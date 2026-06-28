# FFI layout query snapshots

## Context

`ffi.sizeof`, `ffi.alignof`, and `ffi.offsetof` still serialized existing
ctype/cdata layout reads through the cparser token. That was safe, but it kept
stable layout queries on the parser lock path even when no parsing was needed.

String type expressions still need the parser. `ffi.sizeof()` and
`ffi.alignof()` currently keep their locked parser/layout path for strings.
`ffi.offsetof()` now parses the string under the parser lock, then uses the
same sequence-checked layout snapshot for the read-only field walk.

## Change

`lib_ffi.c` now has a sequence-checked layout snapshot reader for stable ctype
layout arguments:

- Begin by acquire-loading `CTState.parse_token`; odd means retry under lock.
- Snapshot `top` and the RCU-published CType table header.
- Bounded-walk raw/ref/attribute, VLA/VLS, info/alignment, and field/subtype
  chains through acquired CType field loads.
- Recheck `parse_token`; any overlap with parser mutation retries under the
  existing locked path.

The existing locked path remains the fallback for active parser windows,
racing table growth, inconsistent chains, and string `sizeof`/`alignof`
queries. The locked `ffi.sizeof()` and `ffi.offsetof()` fallbacks mirror the
snapshot path's payload discipline by reading branch-critical `CType.info` and
`CType.size` values through `ctype_info_acq()`/`ctype_size_acq()`.

## Coverage

Added `tests/t-ffi-layout-snapshot.c`, wired into `m7_ffi_typeinfo_snapshot`.
It verifies stable `sizeof`, `alignof`, `offsetof`, bitfield `offsetof`, and
VLA `sizeof(ct, n)` do not advance the cparser sequence. It also verifies
string `ffi.offsetof("struct ...", field)` advances the parser sequence by one
lock/unlock pair only, proving the read-only field walk does not reacquire the
parser lock.

`tools/ci/m7_ffi_typeinfo_snapshot.sh` also rejects raw `CType.info` and
`CType.size` reads in the `ffi.sizeof()` and `ffi.offsetof()` layout query
bodies.
