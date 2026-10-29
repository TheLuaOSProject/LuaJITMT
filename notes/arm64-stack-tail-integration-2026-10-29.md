# ARM64 stack-aggregate and tail-return integration

The next integration combines full-width integer stack aggregates
(`be1bd66b0`), aligned wide HFA stack groups (`23b375129`), and the bounded
captured-leaf tail-return root (`adeb447fd`) on the validated scalar ITERL
integration. The recorder merge preserves the existing ITERL tuple and RET0
classification, while allowing CALLT only through the separate FUNCF tail
shape. The redundant preprocessor branches around identical preflight calls
were removed without changing their behavior.

Independent review cleared the tail-frame caller-edge question: a Lua caller
entered by C pcall legitimately carries FRAME_C, which describes how it
returns to its own caller. The current child FRAME_LUA edge, prior Lua closure,
its prototype, and the exact source call authenticate this transition.
Requiring frame_islua(caller) would reject that valid path.

Combined validation is complete at `78e43659b` (production `c41600c86`). The
full gate passed across the initial run and the continuation described below;
this was not one uninterrupted run. The ordinary ARM64 and x86-64 builds each
passed all 509 stock source cases and all 509 imported-bytecode cases, plus
snapshot-position, varargs, topology, parity-inventory, and aggregate probes.
Strict GDBJIT compilation passed for both trace and assembler units on ARM64
and ARM64e. The ordinary ARM64 assertion profile was restored afterward.
Evidence, frozen executable/archive/library copies, and their hashes are under
`/Users/frityet/Scratch/ljmt-stack-tail-integrated-20261029/`.

This checkpoint does not establish full architecture parity. Nil-terminated
ITERL, wider side graphs, additional calls/returns, and broader FFI aggregate
ABI shapes remain separate work.

## First combined gate boundary

The run at `c41600c86` passed the preceding no-JIT/GC2/emitter/spill/FFI
contracts, then stopped before the IR admission fixture because its source
check required the former `J->pt` assignment. The tail root intentionally
uses the pinned trace start prototype via a local admission-state copy.
Both affected contract scripts now check that path. The semantic-before-
allocation and post-RA-before-marker shell assertions were made explicitly
fatal when any predicate fails. This is a test-only correction; production
source remains `c41600c86`. The continuation starts at the IR admission
contract, preserving the first log separately.
