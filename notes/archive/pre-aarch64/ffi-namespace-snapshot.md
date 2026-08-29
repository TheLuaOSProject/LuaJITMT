## FFI Namespace Snapshot

Stable `ffi.C` namespace lookups no longer take the cparser token on the
normal path. `lj_ctype_getname_snapshot()` sequence-checks the ctype name hash
chain, copies the matched `CType`, and optionally snapshots the first
`CTA_REDIR` sibling name used for `asm("...")` external symbol redirects.

Interpreter `lj_clib_index()` uses the snapshot/wait path for namespace lookup.
Constants now also snapshot the child integer type before converting the value,
so the post-lookup child read does not directly reread the shared ctype table.
The recorder `recff_clib_index()` uses the same snapshot discipline for cached
`ffi.C` constants/externs and aborts recording across active or overlapping
parser rollback windows.

Coverage:
- `tests/t-ffi-namespace-snapshot.c` checks uncached constants, uncached
  functions, redirected symbols, and traced cached `ffi.C` access do not
  advance `CTState.parse_token`; it also holds the parser token while resolving
  a constant to cover the native wait/retry path.
- `tests/t-ffi-cparse-rollback-reader.lua` continues to verify `ffi.C` cannot
  observe constants from a failed parser transaction.
- Stock `lib/ffi/redir.lua` was run to cover existing redirected function and
  extern variable behavior.
