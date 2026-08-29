# FFI ctype representation waits

Runtime FFI diagnostics now use `lj_ctype_repr_wait()` instead of calling
`lj_ctype_repr()` directly. The wrapper waits while `CTState.parse_token` is
owned, builds the representation, then accepts it only if the same even parser
sequence is still current.

Predefined ctypes (`CTID_NONE..CTID_CTYPEID`) bypass the wait because those
records are immutable and below the parser allocation boundary. This preserves
the no-wait direct/predefined FFI paths used while another thread owns the
parser token.

This keeps parser-owned callers in `lj_cparse.c` on the raw representation
helper while making non-parser error/tostring formatting use the same
sequence-boundary discipline as the rest of the FFI metadata readers. It covers
conversion errors, cdata arithmetic errors, `ffi.__index`/`__newindex` missing
member diagnostics, bad call/metamethod diagnostics, and cdata/ctype
`tostring()` fallback formatting.

Coverage:

- `tests/t-ffi-tostring-snapshot.c` holds the parser token while formatting a
  ctype object, plain struct cdata, enum cdata, and existing reference/pointer
  cases.
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
