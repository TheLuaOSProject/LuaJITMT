# FFI Double 64-Bit Mixed C-Call Helpers

`lj_ccall_jit_num_num_i64()`, `lj_ccall_jit_num_i64_num()`,
`lj_ccall_jit_num_num_u64()`, and `lj_ccall_jit_num_u64_num()` extend the
helper-backed x64 ordinary FFI ccall recorder to exact mixed FP/GPR calls with
a double return and one signed or unsigned 64-bit cdata argument.

The recorder requires one exact `double` argument and one exact 64-bit integer
ctype argument, checks for `IRT_I64` or `IRT_U64`, and selects the helper that
matches both argument order and signedness. The foreign call remains wrapped in
the shared `CCallNativeState` save/enter/leave/checkstop protocol and keeps the
generic direct `IR_CALLXS` path disabled.

Focused coverage traces `double,int64_t`, `int64_t,double`,
`double,uint64_t`, and `uint64_t,double` shapes with low-byte-distinct cdata
values.
