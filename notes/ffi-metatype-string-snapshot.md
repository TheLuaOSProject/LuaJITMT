FFI metatype string snapshot handoff
====================================

`ffi.metatype("ctype", mt)` still parses string ctype declarations through the
cparser sequence, because abstract declaration parsing can intern CTState
entries.  After a successful parse, validation now uses
`lj_ctype_info_snapshot()` / `lj_ctype_info_wait()` to read the committed type
kind and raw ID instead of directly reading the shared ctype table.

The actual metatype install remains a CTState mutation through
`lj_ctype_setmeta()`.  This change only narrows the post-parse read window: the
parser token is not extended over read-only validation, and active parser
rollback is handled by the same native wait/retry path as other stable FFI
ctype readers.

Coverage lives in `tests/t-ffi-metatype-snapshot.c`, wired through
`m7_ffi_metatype`.  It checks that a string metatype moves the parser sequence
only for the parse and that ctype-object metatype validation does not move the
parser sequence.
