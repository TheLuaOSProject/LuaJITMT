# ARM64 Lua leaf sequential comparison branches — 2026-10-30

The bounded leaf source recognizer and the semantic and post-RA admission passes now accept a sequential chain of up to four numeric comparisons. Each comparison has one pure arm ending in `RET1` and one forward continuation to the next comparison; the final continuation reaches one common `RET1`. Source paths and cold snapshot edges are decoded from bytecode and matched to the comparison guard. The existing single-comparison branch grammar remains in place.

The native fixture adds `if a < 0 then return a*-1 end; if a > 10 then return a*.5 end; return a+.25`. It exercises both cold comparison exits, verifies same-trace and GC reuse, and mutates the second comparison's source operand in both independent admission passes. Its exact loaded prototype has 18 bytecodes; the three `RET1` words are `0x0002014c` (A=1, D=2), consistent with `BCINS_AD(BC_RET1, expr_toanyreg(...), 2)`. The A field is the zero-based register; D encodes the one-result return count.

Validation: `tools/ci/arm64_jit_lua_leaf_contract.sh` passed all four ARM64 and ARM64e assert and release profiles. Each return fixture run reported `cases=99`, `native=413`, `terminal=96`, `sequential-maps=1`, and `gc=99`; the ordinary sequential case reported one publication, two numeric ranges, six same-trace reuses, and matching results. Full log: `/tmp/leaf-seq-clean-profiles.log`.

This does not admit arbitrary control flow. The bound is four comparisons, paths must be acyclic and forward-only, each branch must have the described early-return/continuation shape, and comparison arms must stay within the existing pure-operation grammar. More general joins, loops, calls, and other return layouts remain closed.
