lj_ccall aggregate classification helper loads

- Routed the x86_64/POSIX aggregate classifier through helper-backed
  `CType.info`, `CType.size`, and `CType.sib` reads for array element walks,
  nested struct classification, field offset accounting, bitfield checks, and
  stack-overflow alignment fallback.
- Routed the default pass-by-value struct alignment macro through
  `ctype_info_acq()` so the common argument stack alignment path no longer
  reads the alignment bits directly from `CType.info`.
- Follow-up small-struct overflow cleanup caches the x86_64/POSIX stack
  alignment before `ccall_struct_arg()` calls the wait-capable
  `lj_cconv_ct_tv_l()` conversion helper. The stack spill path no longer
  rereads the raw destination `CType *` after conversion may have parked on the
  parser token.
- Follow-up lifetime cleanup threads `lua_State *` through the x86_64/POSIX
  classifier and snapshots each field and raw child record with ccall-local
  wait helpers. Recursive array/struct classification now walks stack-owned
  `CType` copies instead of reopening sibling and child IDs through raw table
  pointers.
- Documented why x86_64/POSIX aggregate classification keeps its CType and
  converted-payload accesses behind the helper surface: a wait-capable
  conversion may park on parser ownership, so later classifier reads need a
  stable acquisition model. The runnable coverage stays in C-call/native
  behavior fixtures and typeinfo snapshot tests; this note and the helper
  comments carry the classifier rationale.
- `tests/t-ffi-ccall-struct-overflow.c` forces six integer arguments followed
  by a parser-owned small struct argument so SysV x64 spills the converted
  struct to the stack after waiting in native time.

Verification:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m7_ffi_ccall_native`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `git diff --check`
