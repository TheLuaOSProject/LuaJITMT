FFI string layout snapshot handoff
==================================

`ffi.new`, `ffi.cast`, `ffi.sizeof`, `ffi.alignof`, `ffi.offsetof`, and
`ffi.istype` still have to parse string ctype arguments through the cparser
sequence:
abstract ctype parsing interns CTState entries, and `ffi.cdef` remains the
explicitly serialized mutation path.  Once a string ctype parses successfully,
the entrypoints now release the parser token immediately and do all layout
inspection through the existing sequence-checked snapshot/wait readers.

This keeps failed-parser rollback safety without extending parser-token
ownership over pure layout work.  The VLA paths are the visible regression
case: after parsing `"int [?]"`, `ffi.sizeof` and `ffi.new` can read the
runtime element count and re-snapshot the committed ctype ID instead of
reparsing the type string.

Coverage lives in `tests/t-ffi-layout-snapshot.c` and
`tests/t-ffi-cparse-rollback-reader.lua`.  The fixtures check exact ctype
parse-sequence movement for string `offsetof`, `alignof`, `sizeof`, VLA
`sizeof`, VLA `new`, `istype`, and `cast`, and verify that a string cast cannot
surface fields from an abandoned failed cdef.  These are behavior checks rather
than legacy wrappers.
