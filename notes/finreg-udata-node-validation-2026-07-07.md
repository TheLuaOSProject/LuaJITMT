# FINREG Userdata Node Validation

GC2 userdata FINREG nodes are raw side-list records tracking userdata objects
whose metatable finalizer state must be mirrored outside the classic userdata
chain. Active and retired list walkers were reading `next`, `retired_next`,
`active`, or `obj` before proving the node record was still registered memory.

This slice adds `lj_gc2_mem_registered()` guards to:

- close-time GC2 teardown of active and retired userdata FINREG nodes;
- duplicate detection in `lj_gc2_finreg_udata_register()`;
- `gc2_finreg_udata_retire()` and predecessor unlink handling;
- `lj_gc2_finreg_udata_forget()`;
- `lj_gc2_finreg_udata_finalize()`;
- the focused `t-gc2-traverse` helpers that count userdata FINREG nodes.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m8_weak`
- `tools/ci/lua_test.sh m3_gc2_paranoia`
- `tools/ci/lua_test.sh m0_matrix`
