# VM state helper access

This slice removes the remaining C-side `volatile` dependency from
`global_State.vmstate`.

- `global_State.vmstate` is now a plain `int32_t`; C readers and writers use
  `vmstate_load_acq()` / `vmstate_store_rel()`.
- Per-TG trace-root readers use `lj_tg_vmstate_load_acq()`, with a matching
  store helper for C tests and future non-VM publishers.
- The x64 VM and trace assembler still write the same `vmstate` offsets through
  their existing generated-code macros; this slice only changes C-side access.
- `tools/ci/m3_vm_safepoint.sh` now rejects reintroducing volatile global
  vmstate or raw C-side vmstate access in the GC/profiler/JIT root readers.

Validation:

- `tools/ci/m3_vm_safepoint.sh`
- `tools/ci/m3_gc2_scaffold.sh`
