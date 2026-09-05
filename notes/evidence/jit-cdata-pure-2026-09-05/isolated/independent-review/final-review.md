# Independent pre-MT cdata load-coalescing proof review

Reviewed production commit `b4e26564542cb8bfa997a11c6a90e5e0017a2f79`, the original design note, and the initial and revised candidates qualified below. Frozen production source and both reconstructed candidate boundaries are identified by `final-manifest.json`. This is a bounded independent source review, with no compilation, runtime execution, broad tests, performance measurements, or shared edits. It does not independently validate the separate native remote-ack protocol under review by root.

No new candidate correctness blocker was found in the reviewed paths. The candidate is narrower than the original prose: an explicit protected root-unroll scope and a direct cdata payload store shape, rather than arbitrary scalar raw stores. Two activation claims in the original note need correction before they can support a landing decision. An in-flight first foreign attachment can wait for a natural native exit, and the global GC-worker API can start workers without flushing traces. These facts do not by themselves supply a stale-method counterexample; their safety arguments and progress limits differ.

## Final source qualification and follow-up

The final reviewed candidate is patch `508e80124110ef8bfbf9ecbedfa646afdce846d1f78df79f82b0ee233889000f`. `final-manifest.json` identifies both qualified candidates. Each was reconstructed independently from the frozen b4e26564 production files plus its exact patch and all three resulting file hashes match the owner's corresponding manifest. No code was built or executed in this audit.

The initial candidate only recognized KINT offsets and therefore refused the owner's positive native cdata loop, which x64 represents with KINT64. The revised helper also accepts KINT64 only when its unsigned 64-bit value is between 16 and INT32_MAX inclusive. Every negative signed representation is above INT32_MAX and rejects; this exactly preserves the nonnegative, int32-bounded direct-payload offset interval. The base and stored type restrictions are unchanged, as are the full-body allowlist, private unroll scope, and FLOAD exception. No new blocker was found in this representation adjustment.

Artifact assembly crossed an owner edit: the earlier `manifest.json`, `candidate.patch`, and `candidate-source/` captured an intermediate signed-field draft (patch `285f4c471b6defadedb5fc36eed3aab7229689cfb2a83ef8b8c92c478ec44d3e`), rather than the already-read initial patch named by `review.md`. Those initial copies are preserved as captured, not used as validation evidence. The `qualified-initial/` and `qualified-revised/` reconstructions remove this ambiguity. Use this final review and `final-manifest.json` for the handoff.

Root is independently probing first foreign attachment during a finite warmed mode-0 loop; this audit did not duplicate that probe. Root also identified that a global SCAN_ROOTS request cannot be presumed safe across arbitrary recorder scratch growth (raw irbuf/snapbuf roots). Nothing in this optimizer flag establishes a new compiler/root-scan lifetime contract. The reviewed native-body exclusions must remain, and separate native-ack/recorder root audits remain independent prerequisites for any broader cutover.

## Exact interval and compiler scope

The reusable proof covers the original guarded root pre-roll and subsequent iterations that remain inside that root self-loop. It does not cover interpreter execution, a helper or foreign call, an arbitrary callback, or an unguarded entry into another trace. All original pre-roll instructions are classified, including operations later than a candidate lookup in the previous iteration.

The candidate requires root ownership (`parent == 0`), self linkage, the full `lj_record_mt_runtime_shared` predicate false, and an emitted mode-0 XPOLL. Its FLOAD exception recognizes only the typed cdata base-root GG field and TAB_NODE reached from that exact typed FLOAD. It also rechecks the full shared predicate, the mode-0 poll, and the private compiler flag. XBAR and ordinary field/table alias searches stay in force; the general poll alias limit, stores, barriers, and other table loads are unchanged.

`lj_trace.c:6314` invokes loop optimization only for a self-link at matching frame/return depth, after DCE. The private byte is initialized to zero before `lj_vm_cpcall`, set only while `loop_unroll` classifies/copies the body, and cleared immediately on protected return before substitution-buffer free, loop rollback, recording retry, or error propagation. Snapshot allocation may fail while the byte is set, but the protected-call return clears it before any subsequent optimizer work. The byte is not a runtime exclusion or atomic policy field. The JIT token owns it. No flag-lifecycle leak was found; it must not be used outside this scope in a later extension.

The classification is of the surviving executable IR, not a promise that the source Lua function makes no calls. A statically inlined Lua function is acceptable only if its resulting operations meet the same allowlist. There is no arbitrary Lua call boundary left in that accepted native body.

## Hidden lowering and effect inventory

| Candidate class | Reviewed x64 lowering and limit |
| --- | --- |
| NOP | No runtime operation. |
| SLOAD | Stack load, tag test, and optional inline numeric conversion (`lj_asm_x86.h:3152`). No runtime helper. |
| FLOAD / XLOAD | Direct memory load and address materialization (`2439`). The exception changes only the two named FLOAD shapes; other loads retain their existing alias rules. |
| HREFK | Node retirement and constant-slot bounds/key guards plus address computation (`2286`). It compares the constant key bits without a string helper or collision-key header traversal. |
| HLOAD | Direct value/tag load and guard (`2539`), with direct fused address operations. No store helper. |
| ADD / ADDOV | FP/GPR arithmetic or LEA; overflow is an ordinary guard (`3521`, `3634`, `3720`). Pointer address arithmetic itself has no callback or allocation. |
| EQ / NE and admitted numeric comparisons | Machine comparison and guards (`3817`, `3990`). String identity EQ is not a string helper. |
| The two exact int32/number CONV forms | SSE/GPR conversions, with checked conversion guards (`1881`); no helper. |
| Accepted XSTORE | Direct scalar machine store (`2474`), further constrained as described below. |

None of these accepted lowering paths increments `gcsteps`. There is consequently no allocation-driven `lj_gc_step_jit` inserted at the loop boundary or trace head by these operations (`lj_asm.c:1835`, `2836`). Root stack adjustment and spill/PHI moves do not allocate Lua objects. Loop unrolling can add PHIs, renames, and the same inline int/number repair conversions; those do not introduce a hidden helper into an accepted body.

The initial explicit rejection is necessary. IR flags alone are insufficient: CNEWI/SNEW can allocate despite their CSE treatment, NEWREF calls a helper, buffer operations can allocate, integer DIV/MOD/POW use helpers, and FPMATH includes both direct instructions and helper calls (even FLOOR/CEIL/TRUNC depend on the SSE feature path). The candidate rejects those whole classes. This review is not approval to add them or to accept generic conversion/arithmetic labels in a future patch.

## Scalar XSTORE and supported alias semantics

The actual candidate accepts only NUM or INT XSTORE whose address is `ADD(cdata_base, positive_constant)`, where the base is a typed SLOAD or KGC and the constant is at least `sizeof(GCcdata)` (16 here). Thus the accepted store writes directly into a cdata allocation after its header. It cannot write the cdata `ctypeid` or flags in the header, and it cannot follow a loaded foreign pointer or cdata-reference indirection into another object. An indirect address derived from XLOAD, an arbitrary pointer base, a Lua-object store, or a pointer-valued store is rejected.

For valid in-bounds FFI accesses, this does not alias the global base-root slot, a Lua table header, a node vector, a method TValue, or a trace KGC root. A legal union may change overlapping scalar payload fields, but those are still fields in that cdata payload. A reference cdata accesses its referent through an additional pointer load and is excluded. CType constructor payloads are not made writable by normal field access. No supported metadata-mutating scalar store shape was found.

The constant lower bound is not a bounds checker. Forged out-of-range indices, pointer arithmetic outside the cdata allocation, writes into opaque VM internals obtained through private casts, and arbitrary host memory corruption are not covered. The native recorder's type/field semantics supply valid extent, not this optimizer predicate alone. Existing FFI strict-alias semantics do not justify claiming every generic XSTORE is confined to a cdata payload; the candidate's structural restriction avoids that broader claim.

## Root and side reentry

Ordinary VM entry first checks the phase gate, publishes exact TG `jit_base` with an SC operation, rechecks the gate, checks the trace slot/start identity and entry/retirement flags, then jumps to the trace's mcode entry (`vm_x64.dasc:6232`). Original root load/node/key/type/method guards remain in that pre-roll. Base-root replacement/removal through `lua_setmetatable` flushes before changing the root (`lj_api.c:2648`); mutation of an existing base table's entries still requires those entry guards and must not be treated as permanent immutability.

Side publication sets the parent exit target to the side trace's mcode entry (`lj_trace.c:6009`). A linked side tail targets the destination trace's `mcode`, not its `mcloop` (`lj_asm_x86.h:4322`). A root is compiled independently of side snapshot inheritance, so reentering it runs its original pre-roll. Side traces are ineligible for the new exception; an inlined mutation or call on a side remains subject to current side alias/entry rules. No path in this candidate was found that links a side directly into the cached root loop body.

A failed XPOLL leaves the root via its snapshot exit machinery. Do not strengthen that statement to assume every guard always reaches the interpreter: Linux trace exit stubs use mutable indirect exit targets. The current C exit path suppresses new hot-side recording while GC needs an exit, profiling is pending, or a poll is pending (`lj_trace.c:6834`). Actual already-installed side chains remain a required test boundary. If such a chain returns to the root, it must hit root mcode and revalidate before the cached body. The implementation owner is providing the low-hotexit and forced-phase-exit controls; this static review is not a substitute for them.

## First activation while mode-0 native code is running

### Foreign Lua/native attachment and MT

A first external attach raises `mt_entering`, publishes a provisional TG, increments the TG count, claims its carrier state, and only then calls `threading_gc_enter_counted`. The first MT transition acquires the JIT token and completes the trace flush before sticky `mt_active`/live admission. The provisional attach path does not call the user's callback before that transition (`lib_threading.c:2369`, `1295`, `820`, `855`; `lj_tg.c:635`). The token spans flush and sticky policy publication, preventing a new pre-policy recording in that gap.

There is a concrete progress counterexample to a universal immediate-exit claim: a warmed mode-0 pure root loop runs indefinitely while a foreign thread requests its first attach. `asm_xpoll` reads only `jit_phase_gate` in mode 0. `lj_safepoint_handshake(EXIT_TRACES|FLUSHJ)` publishes the separate poll word and waits; it does not close the phase gate. If no GC close or natural guard/return occurs, the old trace need not acknowledge that request. The entrant remains before user Lua/callback admission, so this is not yet a stale-method safety counterexample. It is existing activation responsiveness debt. A finite loop can prove request-before-exit and activation-after-exit ordering, but must not be described as proving bounded poll responsiveness.

### GC-worker activation

The L-based worker API flushes while holding the token before publishing worker policy. The global `lj_gc2_workers_set(g,n)` path deliberately calls `gc2_worker_prepare_traces_l(NULL)`, which does no flush (`lj_gc2.c:2258`, `2409`, `2584`). The assembler already documents a foreign controller publishing a worker while a native trace is active (`lj_asm_x86.h:1068`). Therefore the original note's universal worker-flush premise is false.

The narrower safety proof is phase exclusion. MARK activation closes the gate with SC ordering and defers before state/queue changes while `jit_base` is active (`4145`). A MARK worker similarly closes and returns before traversal (`22408`). WEAK begins only with a closed gate and zero active traces and has no native reopen (`4433`). SWEEP before READY remains closed; a READY SWEEP worker closes and defers before reclaim (`22438`). IDLE metadata reclamation owns an exact gate close and validates zero readers/native activity before freeing table vectors, strings, ctypes, or traces (`7033`, `7142`). A mode-0 XPOLL still sees that gate. A closure too brief for the trace to observe cannot authorize destructive progress if the owner observed the trace active.

Worker finalizer work before the traversal gate is a linked MPSC-to-ring splice (`4678`, `22314`), not Lua dispatch. The worker main loop has no Lua callback call (`2758`). Ordinary finalizer stepping refuses active native traces (`4931`); dispatch requires an owner-state claim and no local trace (`4867`). These facts support the accepted pure interval without assuming the worker count stays zero forever. This is a review of the named consumers, not a fresh proof of all GC2 protocols.

### Profiling and arbitrary callback boundaries

On x64 Linux, profile start publishes its recording policy before flushing and does not arm the timer until that flush succeeds (`lj_profile.c:1323`). SIGPROF publishes samples/request atomics; callback delivery is in ordinary owner context (`1049`, `413`, `441`). A sanctioned start from the active owner necessarily occurs outside an accepted call-free body; a different attached Lua owner has already crossed the first MT admission boundary. Such activation does not justify mutating Lua state from an unattached foreign actor or directly from a signal handler. No accepted instruction directly invokes a profile or other Lua callback.

This is the exact native-scope argument. "Single main TG" alone is not an invariant across calls, profile delivery, finalizers, side effects, or foreign activation. All those boundaries must either be excluded from the body, wait for its exit, or reenter through fresh guards.

## Remaining validation boundary

Before landing, retain an emitted IR/mcode positive witness with the original entry guards and unconditional phase XPOLL; eligible direct scalar receiver mutation and weak-method lifetime controls; real native low-hotexit side linking; active phase-close plus reentry; finite first-attach ordering; first global worker activation without an L-based flush; and profile activation/callback delivery. Negative native bodies must demonstrate refusal for allocation/helper/callback/Lua-object writes and indirect-pointer XSTORE. Error/unstable-loop retries should confirm the compiler flag is zero afterward. The original stale-method negative remains the control for entry-guard preservation.

No test or performance result from the implementation owner was rerun or claimed here. Full runtime nonblocking progress remains open, particularly mode-0 poll-only activation and the separately tracked collector/reclaimer/helper dependencies.
