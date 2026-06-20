# Main/VM thread acquire roots

Added `mainthread_acq()` and `vmthread_acq()` helpers in `lj_obj.h` so runtime
users can load the immutable `mainthref` and `vmthref` roots with explicit
acquire semantics.

Routed selected runtime call sites through the helpers:
- legacy and GC2 global root scans;
- userdata list relink/unlink paths and finalizer callback assertions;
- VM event dispatch, safepoint trace flushing, callback slot clearing;
- threading main-thread identity checks, `lua_pushthread()`, and close/free
  assertions.

The raw `mainthread()` and `vmthread()` macros remain for startup/static legacy
uses and low-level tests that intentionally inspect the root list shape.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m3_safepoint_handshake.sh`
- `tools/ci/m4_threading_api.sh`
- `tools/ci/m4_threading_smoke.sh`
- `tools/ci/m7_ffi_callback_runtime.sh`
- `tools/ci/m8_weak.sh`
- `tools/ci/m9_m10_gc.sh`
