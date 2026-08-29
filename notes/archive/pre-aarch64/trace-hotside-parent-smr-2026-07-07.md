# Trace Hot-Side Parent SMR

After `lj_trace_exit()` restores interpreter state it leaves the exit-restore
SMR read section before deciding whether the side exit is hot enough to start a
side trace. `trace_hotside()` then reloaded the parent trace from the public
trace vector and read the parent snapshot counter before it held the JIT token.

That pre-token window can race with trace flush/reclaim. This slice makes the
hot-side decision follow the same lifetime rule as exit restore:

- enter an SMR reader before loading the parent trace from the trace vector;
- use `traceref_safe()` for the parent and root trace lookups;
- update the parent snapshot hot-exit counter while the parent body is protected
  by the SMR reader;
- after the JIT token is acquired, revalidate the parent trace under the same
  reader, publish recorder state, then leave SMR before calling `lj_trace_ins()`.

Once recording starts, the JIT token and trace state continue to provide the
existing flush exclusion for recorder-owned trace body accesses.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m5_jit_trace_publish`
- `tools/ci/lua_test.sh m6_jit_util_flush_race`
- `tools/ci/lua_test.sh m6_jit_flush_hs`
- `tools/ci/lua_test.sh m0_matrix`
