TG JIT exit-code helper surface
===============================

Status: implemented and guarded.

Changes:

- Added `lj_tg_jit_exitcode_*` helpers around `TGState.jit_exitcode`.
- Routed the x64/Linux trace-unwind error-code handoff through release stores
  and acquire reads.
- Kept the non-x64 fallback `J->exitcode` path unchanged.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m6_jit_flush_hs` behavior/counter fixtures; the helper comments carry the ordering rationale.

Validation:

- `make -C src -j$(nproc)`
- `tools/ci/m6_jit_flush_hs.sh`
- `tools/ci/m3_vm_safepoint.sh`
