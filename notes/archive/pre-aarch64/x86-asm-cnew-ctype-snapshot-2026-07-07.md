# x86 ASM CNEW CType Snapshot

`asm_cnew()` now reconstructs `CNEW`/`CNEWI` allocation metadata through
`lj_ctype_info_predefined()` or the sequence-checked
`lj_ctype_info_snapshot()` helper instead of calling the live `lj_ctype_info()`
walker during backend assembly.

The x64 allocation code still receives the same size and alignment bits when
the ctype table is stable. If a parser publish is active or the snapshot races
ctype table growth, assembly aborts with `CTBUSY` and the interpreter can retry
later rather than assembling allocation code from a stale `CType *`.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m7_ffi_jit_cnew`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
