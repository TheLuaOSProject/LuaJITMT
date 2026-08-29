# Amalgam Trace Stale Startins Visibility

`lj_trace_stale_startins()` is called from the x64 VM assembly when an
interpreter dispatch reaches stale `JLOOP`/`JFORL`/`JITERL` bytecode after
trace flush. The DynASM-generated VM object emits an external call for this
helper.

Under the amalgamated build, `LJ_FUNC` intentionally becomes `static` for
ordinary internal C helpers. VM-callable helpers must use `LJ_FUNCA` so their
symbol remains externally visible to `lj_vm.o`. This matches the existing
visibility contract used by `lj_trace_hot()`, `lj_trace_exit()`, and other
assembly-callable C entry points.

Coverage:

- `make -C src amalg` verifies the direct symbol/link contract.
- `m0_matrix` covers the harness build profile that exposed the issue.
- `m6_jit_flush_thread_stress` keeps stale trace/bytecode dispatch behavior
  exercised at runtime.
