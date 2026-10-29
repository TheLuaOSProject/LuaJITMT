# CALLM to a zero-formal fixed consumer — 2026-10-29

A three-result producer followed by a fixed consumer that returns a constant
published natively on x86_64 but failed ARM64 complete post-allocation admission
at the ancestry/arity-normalization integration `fa31c4445`. Source and semantic
admission already passed. The retained loop IR contained a NUM-from-INT
conversion feeding an ADD; both were correctly discarded by assembler DCE
because the consumer ignores all arguments. The allocated proof nevertheless
required the conversion to have a carrier because it saw the retained ADD use.

The scalar no-location proof now recognizes an unallocated pure numeric
ADD/SUB/MUL/DIV terminal with no later IR reference. This permits its input to
lack a location for that use only. A guarded, allocated, spilled or downstream
referenced operation is not such a terminal. Complete snapshot location checks
still run independently. The helper does not change the assembler, recorder,
emitted loop, x86_64 behavior, or publication/lifecycle protocol.

The native leaf fixture now includes three outputs to zero formals. Its private
copies require rejection when the terminal gains a register or spill, or when
a native downstream arithmetic instruction references it. Source/map and
unavailable-extra-argument mutations run in both independent admission passes.
The exact CALLM consumer exit fixture adds a zero-formal consumer swap from
constant 1.25 to 17.25. It requires the selected consumer identity exit, the
full open vector in the exit map, native reentry, and unchanged code bytes,
address, size and generation across GC with a live peer.

Focused ordinary ARM64 execution now publishes the zero-formal root and observes
63 selected exits; the matched ordinary x86_64 probe already did so. Focused
assertion-helper ARM64 checks pass: 40 native leaf runs, 169 semantic negatives,
180 allocated negatives, and eight exact consumer exit cases. Evidence is in
`/Users/frityet/Scratch/ljmt-side-ancestry-20261029/zero-formal-focused.log` and
`zero-formal-fix.log`; the earlier `zero-formal-debug*.log` files preserve the
rejected IR. No diagnostic instrumentation remains in source.

Both complete contracts now pass all four ARM64/arm64e assertion/release
profiles at `0f3bd3286`: `zero-formal-leaf-four-profile.log` and
`zero-formal-consumer-four-profile.log` in the same directory. The worktree was
restored to ordinary assertion ARM64, and all 509 stock source tests plus all
509 original-v2 bytecode tests pass (`zero-formal-stock-source.log` and
`zero-formal-stock-bytecode.log`). The ordinary executable, jit modules,
threading module, source hash and binary hash are retained in `zero-formal-arm64/`.

A follow-up boundary probe confirms a remaining gap for ignored computed
producer results: `(x*.25+1)*.5` and `.5/x` with extra constant results still
interpret on ARM64 while x86_64 publishes the selected loop. The script
`discarded-expression-probe.lua` and both `discarded-expressions-*.log` files
record this. This checkpoint closes the simple discarded terminal calculation;
it does not certify arbitrary dead expression DAGs or full general Lua parity.
