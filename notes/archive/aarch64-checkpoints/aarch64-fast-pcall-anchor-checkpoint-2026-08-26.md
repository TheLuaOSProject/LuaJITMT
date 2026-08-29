# Apple ARM64 fast protected-frame anchor checkpoint

## Closed invariant

ARM64/FR2 fast `pcall` and `xpcall` frames now carry the physical owner TG's
lexical dynamic-root depth in bits 63:32 of their 64-bit delta/type word. The
low word remains the byte delta plus `FRAME_PCALL`/`FRAME_PCALLH`. C frame
walkers use the shared low-word `frame_delta`/`frame_sized` accessors, error
cleanup extracts the high checkpoint, and recorded protected calls inherit the
existing runtime depth guard.

The checkpoint is acquired from `DISPATCH_TG(root_anchor_top)`, not from the
coroutine's migratable `L->tg_hint`. In the current disabled-JIT layout this
field is `0x4cd0` bytes beyond `TGState.dispatch`, which cannot be encoded by one
ordinary AArch64 add-immediate. The VM therefore forms its address from the
layout expression's shifted high 12-bit part and low 12-bit part, acquire-loads
the 32-bit publication, and ORs the zero-extended value into the frame word at
bit 32. The generated sequence is currently:

```
add   x14, x25, #0x4, lsl #12
add   x14, x14, #0xcd0
ldar  w11, [x14]
orr   x21, x21, x11, lsl #32
```

The sequence does not change condition flags. This matters because `pcall`
retains its argument-count condition through the pack and `xpcall` retains its
traceback-type condition. It calls no helper, does not allocate or throw, and
the existing call-frame release publication stores the complete packed word.

## Delta consumers

Two ARM64 assembly paths can consume the protected delta as a full 64-bit
value, and both now mask with `0xfffffff8` before subtracting it from `BASE`:

- common `vm_return`, covering ordinary successful fast returns, resume, and
  protected unwind result delivery;
- `vm_call_tail`, reached when a fast-function fallback returns
  `FFH_TAILCALL`, including `pcall(tostring, object_with___tostring)`.

Lua frames keep a complete 64-bit bytecode return PC. The VM tests the low
frame-type bits before any delta-only arithmetic and never truncates `PC`
itself. Vararg-only relocation paths subtract their word only after proving a
`FRAME_VARG`, which never carries this payload.

## Why this is part of interpreter safepoint correctness

`lj_tab_wait_l()` can service a fresh STOPREQ and throw after retryable rooted
metamethod helpers have closed their SMR/lease scopes but while their dynamic
anchors remain published. A nested Lua fast protected call catches that error
inside an outer successful C protected call, so the outer wrapper cannot repair
the abandoned roots. The fast-frame checkpoint is therefore required for the
ARM64 interpreter safepoint claim; bytecode progress polling alone is not
sufficient. Yield remains intentionally different because the protected frame
and its lexical scope remain live across suspension.

## Deterministic gate

`m5_arm64_pcall_anchor_runtime` runs only against a native macOS ARM64
disabled-JIT bootstrap. It holds two sentinel tables solely in nested TG
anchors, then executes under that nonzero ambient depth:

- successful `pcall` and `xpcall` result returns;
- direct `pcall(tostring, obj)` and `xpcall(tostring, handler, obj)` fallback
  tail-calls through `__tostring` with a full collection in the metamethod;
- nested catches of synthetic fresh STOPREQ, OOM, and table-overflow errors;
- exact post-catch depth, sentinel retention, abandoned-root collection, and
  final LIFO restoration to the original depth;
- a synthetic packed frame word proving high checkpoint extraction and
  low-word delta/type accessors.

`arm64_pcall_anchor_contract.sh` additionally pins the source macro and both
constructor sites, resolves the current TG offset rather than baking it into
the gate, bounds disassembly by symbols, requires both `ldar`/upper-word pack
sequences and both low-word masks, verifies archive/object identity and
freshness, and rejects atomic runtime helpers.

## Existing x86-64 debt (not changed in this ARM checkpoint)

The x64 common protected return already zero-extends `PCd` before its delta
arithmetic. Its `vm_call_tail` fallback reconstruction still copies full `PC`
into `RB` and masks only the low three bits. Under a nonzero ambient anchor,
the same `pcall(tostring, object_with___tostring)` topology can therefore
subtract the packed high checkpoint from `BASE`. The minimal correction is to
replace `mov RB, PC` with `mov RBd, PCd` before `and RB, -8` in
`src/vm_x64.dasc`.

This ARM-only checkpoint deliberately does not change x64. The new runtime gate
is ARM-positive and does not constitute cross-platform proof, while the port
roadmap requires unrelated x64 cleanup to remain separate. The x64 fix should
be a focused invariant commit with the same nonzero-ambient fallback test run
on x86-64 before broadening any shared suite claim.

## Validation boundary

Validated natively so far:

- DynASM regeneration and assert bootstrap compilation;
- emitted `ldar`/pack and low-word mask inspection;
- dedicated source/object/archive contract;
- dedicated no-JIT runtime fixture.

JIT-enabled ARM64 recording, side exits, and snapshot replay remain part of the
later P2 port. Widening `LJ_FRAME_PCALL_ROOT_ANCHOR` activates the existing
record-time runtime-depth equality guard, but this checkpoint does not claim
that unported ARM64 JIT path as executed.
