# VM state helper access

This slice removes the remaining C-side `volatile` dependency from
`global_State.vmstate`.

- `global_State.vmstate` is now a plain `int32_t`; C readers and writers use
  `vmstate_load_acq()` / `vmstate_store_rel()`.
- Per-TG trace-root readers use `lj_tg_vmstate_load_acq()`, with a matching
  store helper for C tests and future non-VM publishers.
- The x64 VM still publishes interpreter/C/exit states to both TG and global
  vmstate mirrors, but Linux/x64 trace heads now publish positive trace numbers
  only to `TGState.vmstate`. Trace exit/unwind, GC trace-root marking, and the
  profiler native-state sampler read the TG vmstate first.
- `tools/ci/m3_vm_safepoint.sh` now rejects reintroducing volatile global
  vmstate or raw C-side vmstate access in the GC/profiler/JIT root readers.

Validation:

- `tools/ci/m3_vm_safepoint.sh`
- `tools/ci/m3_gc2_scaffold.sh`
