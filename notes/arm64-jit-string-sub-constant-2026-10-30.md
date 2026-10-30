# Bounded native substring allocation

The ARM64 scalar LOOP/FORL path now admits `string.sub(s, first, last)` with
one guarded formal string, positive literal bounds, a nonempty fixed result,
one fixed three-argument/one-result CALL and one corresponding `IR_SNEW`.
The last literal must fit a positive signed KSHORT. No emitter, allocator,
interning or snapshot-restoration behavior changes, and no locks are added.

The existing ARM64 SNEW emitter prepares the allocation snapshot and calls
`lj_str_new`. Its GCstr initialization marks the object ready and preserves
it before string-table publication. The new semantic and allocated passes
independently bind STRREF/SNEW to the same guarded string, exact constant
offset/length, preceding same-string length guard and the source CALL.
The shared source decoder only describes bytecode geometry. It rejects
explicit jumps and implicit conditional skips into the argument suffix.
Both proofs require the builtin guard to name `FF_string_sub`, including
later loop guards when the optimizer hoists the allocation. Folded argument
snapshots may resume at either preceding KSHORT or at CALL; the exact pure
suffix preserves the function/string slots. The allocated proof also uses
acquire reads and canonical owner/prototype binding. SNEW participates in
the existing live-across-call register checks.

`tools/ci/arm64_jit_string_sub_contract.sh` passes ARM64 and ARM64e, each with
assertions and release. It is wired into the full experimental gate, but
this component run does not establish a new complete-gate checkpoint.
The native Lua fixture covers sixteen combinations of FORL/while, starts
one/two and widths one/two/three/eight, including NUL/high bytes. It checks
selected-root exits without a TRACE observer, short/empty inputs, coercion
and error behavior against a JIT-disabled oracle, live-peer collection,
retained strings and unchanged machine code after GC.

The C companion checks both loop forms and independently rejects malformed
bounds, string/offset/length operands, allocation type/opcode, first and
later callee substitutions, source literals/argument/CALL shape, conditional
source bypass, a clobbered live source register and invalid result locations.
A matching intern hook witnesses `lj_str_new` while the native frame is
active and checks the returned GCstr arena READY bit. This witnesses the
existing-string helper path: interpreter warmup can intern the result first,
so it is not proof of a fresh allocation originating inside native code.

Evidence is in
`/Users/frityet/Scratch/ljmt-string-sub-constant-20261030/`.
`final-four-profiles.log` is the final component run, including source-skip
mutations and the formatted fixture. Ordinary assertion ARM64 was restored
without helpers, profile
`9fab8de9937545c2ab2dab2e2a14b402c2fdbe44ac651a4d95734b2172f9248a`.
Earlier logs retain the failed test-development attempts and do not replace
this final run.

Ordinary validation at `aca64562a` also passes. ARM64 assertions and strict
bare-`-Werror` x86_64 assertions each pass all 509 source cases, all 509
original v2-bytecode cases, 260 workload semantic checks, the topology probe
and all sixteen substring native/live-peer rows. The workload inventory has
selected-root exit witnesses in eighteen of twenty rows on each target.
The `ordinary/` evidence directory retains source revision, logs, matching
JIT modules, binaries, archives and SHA-256 files. It restores ordinary
assertion ARM64 with profile
`b78389a60e617b5927aee49becb901755962d3330bfd57d3990149451248057b`.

This is a bounded admission slice. Dynamic/negative/empty substring results,
other source shapes, broader string allocation and general Lua call/return
topology remain outside it; full x86_64 parity is still incomplete.
