# ARM64 parity audit and broad implementation route

Audit base: `2387b3ed69dc37db4f0c831715610c360e49c6f1`, the clean
`codex/arm64-side-ancestry-integration` worktree. It contains the newer bounded
call/return, multires, iterator, side-topology and FFI work absent from the
shared `v2.1` checkout. The sample below uses that worktree's pre-existing
ARM64 executable; it is not a clean rebuild of this audit commit. The
independent x86_64 sample was not run in this audit.

## Evidence and current boundary

`bench/jit-parity.lua` passes all 260 interpreter-equivalence checks (13 per
row) on the existing ARM64 executable. Eighteen of twenty rows have an
observed root-trace exit. The `table-iterator` and `ffi-i64-arithmetic` rows
have no witness; as the harness says, zero exits mean no witness, not proof of
no native execution. The latest inventory's earlier paired ARM64/x64 result
also reports 18/20 witnesses on each architecture, but that is a recorded
checkpoint, not a fresh x64 run here.

The latest branch has already closed many sample gaps that were open in older
notes: early-return branches, captured nested calls, fixed two-result leaves,
fixed three-result/open `CALLM`, Lua vararg forwarding in bounded shapes, C
FFI variadic suffixes, canonical scalar `ITERL`/early-break paths, HFA cases,
integer/string conversions and scalar side-of-side chains. These results are
bounded contracts; they do not imply unrestricted source or topology support.

The remaining broad gap is call/continuation and trace topology composition.
`lj_record.c:rec_call_setup()` still bounds inlined leaf frames by `framedepth`
(current cap varies for the supported tail-forward path); the recorder and
independent semantic/post-RA admission handle exact source frame maps, not
arbitrary call graphs. The focused open-CALLM child-overflow note states that
each child-footed snapshot admits at most one producer frame, and the nested
call audit identifies the next concrete gap as a multi-frame active chain.
Returns across arbitrary nested frames, recursive or tail calls, dynamic/open
Lua result vectors and unrestricted Lua vararg areas remain outside the
certified surface. The iterator and side-trace gaps are related but distinct:
arbitrary `ITERN`/iterator callbacks, heap-effect children, stitches and
production side-exit unwind are not established. General FFI aggregate/sret,
variadic-cdata and other ABI combinations are also incomplete.

## Are source-shape certificates fundamental?

No per-program or per-shape certificate catalog is required. Runtime safety
does require immutable trace/body and bytecode-generation seals, independent
semantic and post-register-allocation validation, exact frame/snapshot/source
identity, and entry-time lifetime/topology reacquisition. Those mechanisms
protect lockless publication and execution and must remain as capabilities
expand.

The large number of fixtures and narrow predicates is conservative admission
scaffolding, not a required number of runtime certificate formats. Existing
exact CALL maps and independent semantic/allocated validators already show a
reusable proof pattern. The 255-word/frame limits in older scalar FUNCF
predicates are implementation bounds, not ARM64 requirements. A dynamic,
bounded per-trace frame ledger can replace repeated one-frame assumptions;
it must not merge semantic acceptance with allocator/location validation.

## Recommended broad route

Prioritize a generic active-frame ledger shared by root calls and child exits.
For every snapshot, reconstruct the active chain from acquire-copied source
bytecode and exact snapshot footers. Each frame row should bind its function
and prototype, caller base, source CALL and return PC, argument map, normalized
formals, result destination/count, live slots, and continuation PC. The ledger
is a decoding aid, not an admission verdict: the semantic pass independently
proves bytecode/value meaning; the post-RA pass independently proves physical
register/spill coexistence and snapshot recoverability. Seal the accepted map
and source generation at publication as existing traces do.

Advance in this order:

1. Generalize the existing depth-two, fixed-arity, one-result pure Lua call
path to several active fixed-call frames without upvalue reads initially.
Derive arity and maps from source instructions; reject recursion and repeated
frame tuples until there is a termination/liveness proof. Test exits at each
depth, returns that shrink the chain, caller-value GC roots, identity changes,
post-GC reuse, malformed frame footers, and physical aliases.
2. Bind child closure/upvalue reads to the exact frame identity and UV index,
then allow bounded control flow/branches independently at each frame.
3. Expand fixed result counts and open-result flows from the existing 1–3
result/open-CALLM proofs to dynamic result counts only when the exact
`MULTRES`/open-top state and replay-safe continuation can be reconstructed.
4. Generalize Lua vararg frames after representing the vararg vector/top in
that same map. Keep C FFI variadic suffix proofs separate; their ABI promotion
rules do not establish Lua vararg-frame safety.
5. Reuse the frame/continuation ledger for arbitrary iterator roots, then
compose it with heap-effect child traces and stitches. Iterator mutation and
side-exit unwind need their own operation and lifecycle proofs.

The fresh cross-architecture matrix should report semantic checks, selected
root/child entry and exit witnesses, and exact continuation identities
separately. Compare identified ARM64 and x86_64 binaries for each checkpoint;
preserve mutation negatives in both independent admission passes and exercise
GC/retirement around exits. This grows generic proof protocols and removes
source-specific restrictions without weakening lockless runtime contracts; it
does not admit arbitrary Lua effects by default.

## Current sample invocation

`LUA_PATH='src/?.lua;;' src/luajit bench/jit-parity.lua`

Result on the existing ARM64 executable in the audited worktree: 20 rows, 260
semantic checks and 18 rows with root-exit witnesses. `table-iterator` and
`ffi-i64-arithmetic` had no witness. No x86_64 result was collected in this
audit.
