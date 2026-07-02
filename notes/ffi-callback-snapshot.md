# FFI callback set/free snapshots

`ffi.callback_set()` and `ffi.callback_free()` now snapshot the callback
cdata's function-pointer type metadata before looking up the callback slot.
They only need to know that the cdata is pointer-shaped and pointer-sized, so
the shared ctype table no longer needs a direct raw record read on this path.

Freed callback cdata still fails through the existing "bad callback" path
before touching slot state. Live callbacks use `lj_ctype_info_snapshot()` and
fall back to `lj_ctype_info_wait()` across an active parser transaction.

Coverage:
- `tests/t-ffi-callback-snapshot.c` checks callback `set()` does not advance
  `CTState.parse_token` for stable callback cdata.
- The same fixture holds the parser token and verifies both `set()` and
  `free()` wait from a native region before touching callback slots.
- The fixture is wired into `m7_ffi_callback_install`; this is behavior
  coverage, not a source-text check.
