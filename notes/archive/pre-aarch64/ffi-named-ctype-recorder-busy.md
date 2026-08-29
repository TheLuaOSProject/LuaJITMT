## FFI Named CType Recorder Busy Handling

String ctypes that name a typedef, struct tag, union tag, or enum tag read the
global ctype name hash. That hash is still published by the cparser, so readers
must retry while `CTState.parse_token` is odd to avoid observing a partially
published or rolled-back name chain.

`ffi_lookup_named_ctype()` now keeps the snapshot-first path, but routes the
unstable retry through the same `CTBUSY` helper used by FFI layout readers. This
preserves interpreted semantics while preventing the JIT recorder from parking
behind a cparser publication window.

Coverage:

- `tests/t-ffi-recorder-string-ctype-busy.c` now exercises
  `ffi.new("named_typedef", value)` while a trace hook holds `parse_token` odd,
  and requires a `CTBUSY` abort before the token is released.
