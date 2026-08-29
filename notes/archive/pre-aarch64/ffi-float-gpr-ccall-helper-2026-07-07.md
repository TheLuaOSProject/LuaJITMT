# FFI Float GPR C-Call Helper

`lj_ccall_jit_flt_gpr()` extends the traced x64 ordinary FFI ccall helper
surface to exact `float` returns with the shared one- or two-argument GPR
signature matrix used by `lj_ccall_jit_num_gpr()`.

The recorder path is intentionally parallel to the double-returning GPR helper:
fixed arguments only, one or two arguments, exact signed 32-bit, unsigned
32-bit, pointer, signed 64-bit, or unsigned 64-bit argument types, callback
blacklist checks before emitting the call, and the same native-state
save/enter/leave/checkstop protocol inside the helper. The helper returns an
IR `FLOAT`; the recorder widens it to Lua number after the call, matching the
existing `lj_ccall_jit_flt_fpr()` and `lj_ccall_jit_flt_num()` result handling.

Focused shared-library coverage now traces representative float-returning GPR
shapes: `float(int32_t)`, `float(pointer)`, and
`float(int64_t, uint32_t)`.
