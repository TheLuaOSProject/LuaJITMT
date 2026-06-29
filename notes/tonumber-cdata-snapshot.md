# tonumber(cdata) ctype snapshot

`tonumber(cdata)` no longer reads `lj_ctype_rawref()` and raw `ct->info` /
`ct->size` directly. The runtime path now follows raw-ref semantics through the
predefined fast path or `lj_ctype_rawref_snapshot()`, waiting in native time if
another thread owns the parser token. Enum children use `lj_ctype_info_wait()`
before the numeric/complex conversion decision.

Coverage lives in `tests/t-ffi-tonumber-snapshot.c`, wired into
`m7_ffi_typeinfo_snapshot`. The fixture pins the predefined no-wait path and a
parser-owned enum cdata value that must wait without holding the VM.

Verification:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
