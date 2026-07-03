# JIT FNEW prototype call guard

Same-trace calls to a closure just returned by `FNEW` now specialize the callee
by prototype PC instead of by closure object identity. `FNEW` intentionally
allocates a fresh closure each iteration, so an object `EQ` guard on the helper
result always side-exits on the next loop trip even though the prototype and
upvalue layout are stable.

The recorder only applies this to `CALLA lj_func_newL_gc_forjit` values whose
argument tree carries the same prototype pointer as the runtime callee. The
frame still keeps the dynamic closure TRef, so `UREFC` and later upvalue loads
use the fresh closure instance and preserve Lua closure semantics.

Focused regression test: `m6_jit_cell_ops` now requires same-trace `FNEW` immediate
calls to contain `FLOAD <callee> func.pc`, proving prototype specialization
instead of fresh object identity specialization.
