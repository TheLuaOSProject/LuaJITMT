# JIT KGC constant acquire reads

The IR KGC publication path release-stores the GCRef before release-publishing
the `IR_KGC` opcode. GC scanners already use `ir_iskgc_acq()` plus
`ir_kgc_load_acq()`, but two non-scanner consumers still copied/replayed KGC
constants through the raw `ir_kgc()` macro.

Changed:
- added `ir_kgc_acq()`, which acquire-loads the opcode in all builds, asserts it
  is `IR_KGC`, then acquire-loads the GCRef;
- `lj_ir_kvalue()` now uses `ir_kgc_acq()` when materializing an `IR_KGC`
  constant, covering `jit.util.tracek()` and snapshot constant value copies;
- `snap_replay_const()` now uses `ir_kgc_acq()` when replaying parent-trace KGC
  constants.

Left recorder/assembler-owned raw `ir_kgc()` uses alone. Those paths operate on
the current trace under the recorder/assembler ownership model and are not this
published-trace reader cleanup.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m5_jit_trace_publish.sh`
- `tools/ci/m7_ffi_snap_restore_l.sh`
- `tools/ci/m6_jit.sh`

During validation, a stale host `buildvm` initially generated `lj_libdef.h`
without the `jit_opt` block. A clean host-builder regeneration restored the
expected libdef output before the successful full build and tests.
