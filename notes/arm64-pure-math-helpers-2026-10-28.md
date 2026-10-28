# ARM64 pure scalar math helpers

This continuation admits unary `log`, `log10`, `exp`, `sin`, `cos`, `tan`,
`asin`, `acos`, `atan`, `sinh`, `cosh`, and `tanh`, including `log` with a
fixed second argument. It uses the existing ARM64 FPMATH and CALLN lowering;
no emitter or x64 execution changes are needed. The source fast-function
preflight checks fixed scalar argument geometry and exact synthetic bytecode
slots. Semantic and post-allocation admission independently check the NUM
operands and a fixed list of pure unary helper IDs. Other call IDs, effectful
calls, indirect dispatch and side-trace use remain outside this slice.

Pure libm calls keep the executing TG's native GC phase lease. They do not
enter Lua, allocate Lua objects, or publish native pointer locations as GC
roots. The allocated proof additionally checks live values across each call,
including preloop invariants used after a loop backedge, and the exact renamed
locations used by subsequent snapshots. Darwin's preserved GPRs, the low
64-bit values of D8-D15, or certified full-width spill slots must retain the
values. Existing spill ownership, snapshot and PHI checks remain in force.
The existing allocated call-survival helper was moved outside the JIT-FFI
conditional and renamed for this shared machine-state purpose; semantic
admission does not consume its result.

The scalar capability contract passes on ARM64 and ARM64e, each with assertions
enabled and disabled. The final frozen run is
`/Users/frityet/Scratch/ljmt-math-helpers-20261028/four-profile-final.log`.
Each profile includes 37 helper-focused native observations: finite/domain
cases for all twelve functions and a composed sin/cos/log-base calculation.
The composition rejects six source-liveness-selected corruptions that leave
live NUM values only in a caller-saved register. Independent semantic and
allocated mutations also reject effectful PRNG IDs, forward arguments, invalid
flags and unsupported FPMATH IDs. The ordinary companion compares JIT-off and
JIT-on results over finite values, signed zero, large magnitudes, NaN and
infinities. Lifecycle checks cover profile exits, STOPREQ unwinding, resumed
entry, full collection and reuse while a peer TG remains alive.

These generic scalar certificates validate admissible IR semantics and machine
state, not equivalence of every scalar operation to its originating expression.
As with existing ADD/SUB and FLOOR/CEIL admission, substituting one valid pure
unary operation for another is not a malformed structural IR shape. The stock
recorder remains responsible for selecting the expression's operation; native
interpreter-oracle tests check the actual recorded results. No general source
equivalence or full math/JIT parity claim is made.

A follow-up admits NUM FPMATH and unary CALLN results as loop-carried values.
Both certificates accept only the existing `IRT_ISPHI` annotation and still
independently require exact agreement between that annotation and the actual
PHI operands. Twelve repeated-call workloads (`x = op(x)`) now execute natively,
including NaN-producing domains and overflow to infinity. Both certificates
reject toggled PHI annotations. The final four-profile C/Lua run is
`carried-four-profile.log` in the evidence directory above. The ordinary
companion additionally compares repeated calls over its entire edge-value
matrix; this is result evidence, not a claim that every edge trace compiles.
The new repeated-call matrix exposed floor's negative-zero normalization in
the previous ARM64 binary too. Installed stock LuaJIT also loses that zero sign
under JIT narrowing (its invariant scalar case already does so). Accordingly,
the additional floor/ceil recurrence comparisons permit the stock zero-sign
normalization while retaining exact numeric values and NaN classification;
the new helper recurrence cases retain exact signed-zero comparisons. This
is a test-oracle distinction, not a claim to have repaired that stock behavior.

A separate FFI-disabled build exposed existing ARM64 conditional-include and
`ctype_ctsG` call-site omissions. The first diagnostic is retained in
`/Users/frityet/Scratch/ljmt-math-helpers-20261028/noffi.log`. A follow-up makes
the fast-function definitions available to scalar ARM64 admission without FFI
and supplies a null CType context when FFI is disabled. The ARM64 assertion
build with `LUAJIT_DISABLE_FFI` and its ordinary math companion now pass;
`noffi-fixed.log` in the same directory records this result. This comparison
checks results, not a native-entry witness for the FFI-disabled configuration.
The full combined experimental gate and stock corpus will be rerun on the
integration branch. General binary math,
multi-result math, nested Lua frames and broader trace topology still require
their own proofs and tests.
