# Ordinary custom-iterator root comparison

Two new cases in `bench/jit-topology-probe.lua` establish a native ITERL gap
on ordinary assertion builds from production source `6968c4cac`. Both use
only a custom Lua iterator and scalar arithmetic. The runner contains no
other loop. Root IDs come from its own patched bytecode, so a compiled
iterator helper does not count as a runner-root witness.

| Runner | x64 | ARM64 |
| --- | --- | --- |
| Scalar iterator with a break in the body | JITERL root, 18 IR instructions, 5 exits | No runner root |
| Iterator returning nil at the limit | JITERL root, 17 IR instructions, 5 exits | No runner root |

Each runner passes five phases on both targets: a 64-iteration warmup,
an arithmetic/comparison type error, zero and one limits, and a 96 limit.
Each phase compares 80 calls and another call after full collection with a
separately loaded, recursively JIT-disabled oracle. A live peer remains
attached. The first phase observes five selected-root exits on x64 for each
runner; the error and short-loop phases distinguish helper exits from runner
exits. No TRACE observer is installed in this comparison.

The complete expanded 16-case topology probe passes result/error comparisons
on both targets. Missing ARM64 roots are reported, not asserted as equivalent
compilation. This is gap evidence, not an iterator implementation.

Separate table-iterator probes initially created the table inside the runner;
their ARM64 FORL construction traces were unrelated to the iterator. Moving
construction outside the runner removes those false witnesses. The isolated
ipairs, dense/hash pairs and explicit next cases publish no runner root on
either target in this sample. They do not establish an ARM64-only difference.

## Source boundary

The ARM64 recorder and native-entry gates currently admit LOOP, integer FORL
and bounded FUNCF roots. ITERL is refused. Its VM instruction already tests
the returned key and copies it into the control slot. JITERL then follows the
branch-only stale-generation recovery path; native entry must occur after
those effects, without repeating them on rejection.

The generic recorder copies the iterator function/state/control triple into
the ordinary call frame before recording ITERC. Existing ARM64 Lua-leaf
source/frame checks require CALL, so opening the root opcode alone would
still be insufficient. A complete extension must bind the ITERC/ITERL and
setup-jump generation, copied call arguments, exact callable identity,
result/control-slot flow, snapshots and allocated state. Nil termination and
body breaks need separate cold-continuation checks. Side admission must keep
its existing limits until it has its own iterator-parent proof.

Evidence is retained at
`/Users/frityet/Scratch/ljmt-iterator-parity-20261029/`: expanded topology logs,
isolated table/custom-iterator logs, probe sources, and a separate bytecode/IR
dump for the scalar iterator. The dump is diagnostic only because its TRACE
observer changes tracing conditions. Runtime/archive/module bundles are the
immutable `ordinary-fixed/` bundles in
`/Users/frityet/Scratch/ljmt-funcf-siblings-hfa-integration-20261029/`.
