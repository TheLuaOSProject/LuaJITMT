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

## Recorder-layout diagnostic

An isolated recording-only diagnostic at source `6971c0f16` captured the actual
ARM64 iterator maps after the ordinary optimizer. It temporarily allowed the
known iterator source/frame through recording, then unconditionally raised an
error at the ASM boundary for every ITERL trace. It never assembled, published
or entered that root. The diagnostic changes were preserved as a patch and
removed from source immediately after the run. They are not admission proof.
Evidence is `/Users/frityet/Scratch/ljmt-iterator-record-map-20261029/`, including
source copies, runtime/archive, source ID, patch, probe and `diagnostic.log`.

For the scalar iterator source above, ITERC is PC15, ITERL PC16, setup JMP PC5
and body PC6. With root base slot 2 and ITERC A=5, the original hidden triple
is slots 4/5/6, the copied function/result slot is 7, frame link is 8, and
child parameters are 9/10. The identity-guard snapshot at PC15 contains the
updated sum and copied function slot 7, while state/control arguments remain
implicit inherited loads from original slots 5/6. The control load becomes
an explicit updated map entry after unrolling. A CALL-style proof requiring
all copied argument slots in that parent snapshot would reject this legal
layout, and merely dropping the requirement would lose source binding.

Unlike the sampled x64 numeric trace, this ARM64 run specializes the key as
an integer. The iterator's `k+1` becomes ADDOV, creating child-frame snapshots
with base 7 and exact source RET/ADD continuation. The visible result and
hidden control subsequently alias that ADDOV result in slots 7 and 6. Thus
the first implementation needs both implicit-triple reconstruction at ITERC
and the ordinary leaf arithmetic overflow/frame proof; a scalar result type
alone is insufficient. Parent identity, copied argument geometry, source
ITERC/ITERL/setup-JMP generation, allocated replay and VM entry after the
key-to-control copy must all be checked before native publication opens.

The source worktree was restored clean and its ordinary runtime rebuilt after
the diagnostic. That rebuild and topology check passed; its immutable bundle
is `restored-ordinary/` beneath the diagnostic evidence directory, with source
and SHA-256 manifests. The reusable older FORL diagnostic runner also replaced
its top-level runtime/archive during that restore. Its earlier logs and source
copies remain, but those two top-level binaries now correspond to production
`6971c0f16` (notes revision `b160fbc5c`), not the original prototype.
`runtime-provenance.json` there records that distinction explicitly.
