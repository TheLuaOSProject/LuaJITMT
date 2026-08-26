# Apple ARM64 JIT-off ISNEXT publication (2026-08-26)

## Claim boundary

The JIT-disabled Apple ARM64 interpreter now despecializes an `ISNEXT` guard
and its `ITERN` target with complete 32-bit acquire/release bytecode updates.
The target becomes `ITERC` before the guard becomes `JMP`, and every update
preserves the instruction's A/B/C/D operands.

This closes the last known JIT-off interpreter publication gap in Stage 2. It
does not certify the JIT-enabled `ISNEXT`/`JLOOP` arm: that source projection
is preserved instruction-for-instruction and remains part of the later ARM64
JIT port.

## Target-first transition

The old ARM path wrote only the opcode byte and retired the `ISNEXT` guard
before changing its target. A peer could therefore observe the unsupported
`JMP+ITERN` pair, and a byte store could race a full-word bytecode update and
splice stale operands into the instruction.

The JIT-off failure path now keeps the guard address in callee-saved `x27` and
the target address in callee-saved `x28`, saves `L->base` and the bytecode PC,
and calls `lj_bc_publish_op_vm` twice:

1. release-publish `ITERC` into the complete target word;
2. release-publish `JMP` into the complete guard word;
3. reload `BASE`, set `PC` to the target, and redispatch.

`lj_bc_publish_op_vm` acquire-loads the complete current word, replaces only
its opcode field, and release-stores the complete result. Concurrent JIT-off
despecializers therefore converge on the same two terminal words without a
load/byte-store lost-update window.

`ISNEXT+ITERC` is a supported intermediate state: the specialized guard can
still initialize the control variable, while its generic target calls the
current iterator normally. `JMP+ITERN` is not supported, which is why the
publication order is part of the correctness contract rather than an
optimization detail.

On ARM64, the complete guard load supplies the RD field from which the next
bytecode address is calculated. That data/address dependency orders the
subsequent target-word access after the release-published guard. The emitted
contract pins the fixed registers, full-word guard decode, target derivation,
and redispatch chain so this cannot silently degrade into control-only
ordering.

## Deterministic validation

A clean native ARM64 assert bootstrap with `LUAJIT_MT_ARM64_BOOTSTRAP`,
`LUAJIT_DISABLE_JIT`, and `LUA_USE_ASSERT` passed:

- the full interpreter gate: all 387 vendored stock tests, threading API,
  hooks, coroutine handoff/dead-resume, and 320 FFI callback rounds;
- the strengthened metamethod source/object and runtime gates;
- an `ISNEXT` source projection which requires target-first helper calls,
  exact fixed-register ABI, BASE/PC saves, BASE reload, redispatch, no byte
  stores in the active JIT-off arm, and preservation of the deferred JIT arm;
- an emitted-object/archive contract which requires thin ARM64 artifacts,
  byte-for-byte archive-member identity, current inputs, exactly two ordered
  `BR26` relocations with `x28/ITERC` then `x27/JMP`, and the exact helper
  acquire/merge/release body without an atomic runtime import;
- a runtime fixture using separate fresh prototypes: one executes and retains
  the supported `ISNEXT+ITERC` intermediate pair, while the other reaches the
  real wrapper-triggered failure, verifies exact terminal instruction words
  and operands, performs a full GC, and reruns through generic iteration.

The next implementation stage is ARM64 interpreter safepoint polling and
acknowledgement. The JIT remains compile-time disabled until its recording,
patching, invalidation, exit, and safepoint protocols are ported and gated.
