x86 lock-prefix emitter cleanup

- Replaced table-store inline CAS call-site lock-prefix byte emission with a
  named x86 emitter helper, `emit_lockrmro()`, and added symbolic `XI_LOCK`.
- The helper emits the same `lock cmpxchg [base+ofs], reg` memory-form
  instruction while keeping the lock prefix out of the table-store lowering.
- Verification: `make -C src -j2`, direct `-jdump=im` table-store smoke showing
  `lock cmpxchg [rdx], ...`, and `tools/ci/lua_test.sh m6_jit_table_store_helper`.
