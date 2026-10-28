# ARM64 nested Lua-call gap: source and frame audit

## Evidence

`bench/jit-parity.lua`'s `nested-leaf` is:

```lua
local function g(x) return x+.25 end
local function f(x) local v=g(x); return v*.5 end
return function(n) local s=.5; for i=1,n do s=s+f(i+.5) end; return s end
```

The saved native parity inventory reports 13/13 oracle checks on each target. ARM64 has zero selected-root exits; x64 has one live root and two observed exits. As the benchmark says, zero exits mean no witness, not proof of no execution. A separate x64 bytecode dump (`/Users/frityet/Scratch/ljmt-parity-audit-20261028/nested-bytecode.lua`) confirms the shape: the loop closure uses `UGET; ... CALL` for `f`; `f` has one upvalue and uses `UGET; CGET; CALL`, then `MULVN; RET1`; `g` is a fixed unary `CGET; ADDVN; RET1` leaf. The inventory case therefore needs both two call frames and a read of `g` through `f`'s captured upvalue.

## Exact admission blockers at a4acb620

1. `lj_trace_arm64_lua_leaf()` rejects callees with upvalues, GC constants, or nested-prototype metadata. Frozen x64 `jit.util.funcinfo(f)` reports one upvalue, zero GC constants, and `children=false`; the upvalue is the relevant metadata rejection for `f`. Its bytecode also contains `UGET` and `CALL`, which the source opcode walk refuses. `g` itself is a candidate pure leaf. `PROTO_CHILD` is a separate restriction, but it is not implicated by this workload.
2. `rec_call_setup()` snapshots a Lua callee's caller only when it passes that leaf recognizer, the root is FORL, and `framedepth == 0`. When recording reaches `g` from `f`, the frame depth is nonzero, so this block raises `LJ_TRERR_NYIBC` before recording its call snapshot. This is the first explicit nested-call depth gate if the `f` call proceeds through recording.
3. Recorder root preflight has only a direct-child frame grammar. `trace_arm64_lua_leaf_frame()` requires `framedepth == 1` and the previous frame to be the root prototype; nested `g` has `f` as caller and is deeper. Its base calculation handles one call frame rather than walking a frame chain.
4. Semantic and post-RA certificates independently encode the same one-child assumption. `arm64_lua_leaf_ir_args()` and `arm64_lua_leaf_postra_args()` derive a caller CALL and arguments from one prototype view. `arm64_lua_leaf_ir_child()` checks one frame marker, derives one CALL from its return PC, and uses the one-child base formula. Its allocated counterpart describes a two-frame exit, reads one child `SNAP_FRAME`, and resolves that CALL in the root. The ordinary snapshot loops invoke these helpers for a nonzero child footer and reject when they cannot prove it.
5. The intermediate `f` frame's `UGET g` is an additional source/effect boundary. `arm64_upvalue_fnroot_ir()` requires the helper's function root to derive from `IR_SLOAD slot 0`; semantic and allocated snapshot proofs bind the read to the current prototype's UV index and bytecode. Nested support must prove that this root is the exact `f` closure frame, not reuse an unrelated closure's upvalue authority. The existing rooted helper protocol is reusable only after that frame/function identity is established.

No VM exit-handler limitation to exactly two frames was established in this audit: generic snapshot restoration already consumes frame markers. The concrete blocker is that recorder preflight and the semantic/post-RA admission layer certify only the root plus one child. Do not infer multi-frame native safety from generic `lj_snap_restore()` alone.

## Smallest generic next slice

Introduce a bounded, ephemeral call-frame ledger reconstructed from each snapshot footer and the acquire-copied source bytecode. Each row should bind the current function/prototype, caller frame/base, exact CALL PC and return PC, supplied arguments versus normalized formals, result slot/arity, and child frame extent. For the first slice, pass `g` as an explicit function argument to `f` so the frame-chain work does not claim to cover child-upvalue reads. Use fixed one-result calls (`C=2`) while deriving actual arity from each call's `B`: `f(x,g)` has `B=3`, and `g(x)` has `B=2`. This isolates nested frames; it does not close the inventory gap.

- support an acyclic depth-two child chain, fixed ordinary `BC_CALL`, exactly one result, and no tail/vararg/multires calls. Derive argument count from each source call; don't hardcode either arity;
- admit a child CALL only when its callable is an exact rooted visible value followed by the existing identity guard. Keep child `UGET` closed in this first slice; later bind the rooted helper to that exact child closure and UV index;
- keep current pure scalar/branch body grammar at every frame and reject heap mutation, arbitrary calls, cycles and unsupported effects;
- at every executable snapshot, independently replay the source frame chain and maps in semantic and post-RA passes, including every `SNAP_FRAME`, footer return PC, caller CALL result destination, live arguments/locals, and physical coexistence. The child argument vector includes a function TValue, so prove its exact visible function type and guard binding as well as numeric arguments. Reconstruct resume PCs from snapshot footers and source bytecode; do not infer them from trace order;
- derive the active chain per snapshot rather than requiring every snapshot to retain the deepest frame. After a child returns, validate the shrunk caller chain and bind the materialized CALL result before admitting any later caller-frame snapshot;
- preserve the current TG native/GC phase lease through generated execution and restore the full interpreter stack before releasing native activity. A register/spill in any frame is still not a GC root.

This is generic in callee identity, argument values, source registers, constants and arithmetic, rather than a special rule for `f -> g`. The representation can share bounded bytecode/frame decoding; it must not share semantic acceptance, liveness, allocation, or location verdicts. Reject recursive/repeated frame tuples until there is an inductive recursion proof.

The first implementation should test parameter-passed `g` and adversarial variants: alternate `f`/`g` closures causing identity exits at each depth, child overflow followed by exact interpreter continuation in the middle frame, a GC value in a live caller slot, post-GC reuse, malformed nested return/footer links, and physical alias negatives at both frame levels. A later captured-`g` test must cover concurrent upvalue mutation and pass before claiming the original inventory workload. Native claims need live-MT entry/exit counters, not only ordinary Lua publication.

This is a read-only audit and design proposal; no nested-call source admission was implemented or native-verified here.
