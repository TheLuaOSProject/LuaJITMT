2026-06-20

Slice: x86 exit patcher acquire metadata snapshot.

Changes:
- `lj_asm_patchexit()` in `lj_asm_x86.h` now snapshots published trace
  `mcode`, `szmcode`, and `traceno` through `trace_mcode_acq()`,
  `trace_szmcode_acq()`, and `trace_traceno_acq()`.
- The patch scanner keeps the acquired mcode base separate from the advancing
  cursor and uses the snapshot for `lj_mcode_patch()`, vmstate matching, and
  `lj_mcode_sync()`.

Reasoning:
- This function patches an already-published trace body. Other mcode consumers
  such as debug export, unwind mapping, and trace-exit lookup already acquire
  the same metadata.
- x86_64 acquire loads stay cheap but make the publication contract explicit in
  the patching path.

Intentionally left:
- Non-x64 backend `lj_asm_patchexit()` implementations still have their old raw
  metadata reads. Current target is Linux/x86_64.
- Current-trace assembler writes to `T->mcode`/`T->szmcode` remain raw
  pre-publication construction state.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m6_jit_mcode_publish.sh`
- `tools/ci/m6_jit_flush_hs.sh`
