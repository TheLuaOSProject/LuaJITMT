# JIT FNEW same-trace call specialization

Same-trace calls to a closure just returned by `FNEW` specialize by the helper's
constant prototype argument, not by closure object identity. `FNEW`
intentionally allocates a fresh closure each iteration, so an object `EQ` guard
on the helper result always side-exits on the next loop trip even though the
prototype and upvalue layout are stable.

The recorder only applies this to same-trace `CALLA lj_func_newL_gc*_forjit`
values whose argument tree carries the same prototype pointer as the runtime
callee. The frame still keeps the dynamic closure TRef, so closure identity and
upvalue cell semantics remain ordinary Lua semantics.

As of 2026-07-03, the recorder no longer emits `FLOAD <fresh callee> func.pc`
for this case. The helper's constant prototype argument already proves the
callee layout, and the extra runtime reload was redundant. `m6_jit_cell_ops`
now rejects both the old fresh object identity guard and the redundant
fresh-closure prototype reload while still requiring the helper call to be
present.
