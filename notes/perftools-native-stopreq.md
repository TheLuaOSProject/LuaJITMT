2026-06-20

Subject: JIT perf-map writer native STOPREQ boundary.

`LUAJIT_USE_PERFTOOLS` writes `/tmp/perf-<pid>.map` during trace
publication. The first open can block on a FIFO in tests, and in real use it is
filesystem I/O from the mutator thread, so it must be marked native for MT
handshakes.

Important detail: `trace_state()` runs under the recorder's internal
`lj_vm_cpcall()` loop. Throwing `thread interrupted: VM shutdown` inside that
protected callback is converted into trace-abort behavior and does not reach the
outer Lua call. The user-visible STOPREQ check therefore belongs after
`lj_trace_ins()` returns to `lj_dispatch_ins()`, once the trace engine is idle
or no longer owned by the current TG and the VM is back at a normal callback
boundary.

The fixture `t-jit-perftools-native.c` verifies behavior rather than source:
it turns the perf map into a FIFO, observes the perf writer in native state,
delivers a real STOPREQ handshake, confirms the target TG saw `TGF_STOPREQ`,
then asserts the Lua call fails with the shutdown error.

Validation:

- `tools/ci/lua_test.sh m6_jit_perftools_native`
- `tools/ci/lua_test.sh m6_jit_token`
- `make -C src clean && tools/ci/lua_test.sh m6_jit_flush_hs`
