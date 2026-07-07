# FFI Float 64-Bit Mixed C-Call Helpers

`lj_ccall_jit_flt_flt_i64()`, `lj_ccall_jit_flt_i64_flt()`,
`lj_ccall_jit_flt_flt_u64()`, and `lj_ccall_jit_flt_u64_flt()` extend the
helper-backed x64 ordinary FFI ccall recorder to exact mixed FP/GPR calls with
a float return and one signed or unsigned 64-bit cdata argument.

The recorder requires one exact `float` argument and one exact 64-bit integer
ctype argument, checks for `IRT_I64` or `IRT_U64`, selects the helper matching
argument order and signedness, and widens the IR `FLOAT` result to Lua number.
The foreign call remains inside the shared `CCallNativeState`
save/enter/leave/checkstop protocol.

Focused coverage traces `float,int64_t`, `int64_t,float`,
`float,uint64_t`, and `uint64_t,float` shapes with low-byte-distinct cdata
values.
