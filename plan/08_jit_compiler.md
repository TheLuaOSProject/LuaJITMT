# 08. The JIT Compiler Under Multithreading

Principle (ADR-8): **recording is serialized, execution is free.** One
CAS-acquired token gates the recorder/assembler/jit_State; everything any
thread *executes* — published traces, exit handling, hot counting — is
lock-free and per-thread. Published artifacts (mcode, GCtrace bodies) are
immutable (I-6); all retargeting is data: bytecode words, exit tables,
link words.

## 8.1 jit_State stays single-instance

`jit_State` (lj_jit.h:417–520) is a recording workspace: cur trace IR
buffer, slots, snapshot buffers, fold state, scev, penalty cache, plus the
shared trace registry (`J->trace`, `freetrace`, `sizetrace`) and the mcode
area cursors. Splitting it per-thread would mean per-thread mcode areas and
a cross-thread trace registry anyway — high cost, no win (recording is
rare; a hot loop records once). DECIDED: one J, reached via `g->jitp` (03
§3.4), protected by the recorder token for all *mutating* access. Read-only
execution-time accesses (trace lookup by number) go through the RCU rules
of §8.3.

## 8.2 The recorder token

```c
/* in GC2State or g: */ uint32_t jit_token;   /* 0 free, else owner tid */
int lj_jit_token_try(TGState *tg);  /* CAS 0→tid; no spin, no wait */
void lj_jit_token_release(TGState *tg);
```
Acquisition points (all currently funnel through lj_dispatch.c /
lj_trace.c):
- `lj_trace_hot` (lj_trace.c:781) and hotcall path → if token busy:
  **reset the hotcount and keep interpreting** (lock-free: never wait).
- `lj_dispatch_ins`/`lj_dispatch_call` record-mode entries: only the token
  holder ever has `J->state != LJ_TRACE_IDLE` visible for *itself*; other
  threads see their dispatch tables without record hooks (HS_REDISPATCH
  switches only the recording thread's TG table to the recording variant —
  03 §3.6 mechanism reused locally; restore on token release).
- `lj_trace_freeproto/reenableproto`, blacklisting/penalties (penalty_pc,
  lj_trace.c:392): token-held only.
- `jit.flush/on/off`, `lj_trace_flushall`: token + handshake (§8.7).
Token-held sections never allocate from the GC heap while holding? They DO
(GCtrace, ctype, IR growth use lj_mem) — fine: the token is not a lock in
the §2.2 sense (it is never *waited on*; failure = fall back to
interpreting), and allocation inside it may poll/assist normally. Document
the one rule: **safepoint acks must not try to take the token** (they
don't).

Hot counters: per-TG (07 §7.7), so two threads can go hot on the same PC;
the second simply fails the token try and resets its counter — natural
backoff. Penalty slots stay in J (token-held writes; racy *reads* from the
hotcount path are advisory only — acceptable staleness).

## 8.3 Trace registry & publication

`J->trace` is `GCRef *trace; MSize sizetrace` (lj_jit.h:473–475).
- Growth (token-held): allocate new vector, memcpy, `la_storeptr_rel
  (&J->tracev, new)`, defer_free(old) (05 §5.9). Readers (`traceref` —
  redefine to acquire-load the vector pointer once per use site) are
  lock-free.
- Slot publication: `trace_save` (lj_trace.c:145) currently
  `setgcrefp(J->trace[T->traceno], T)` — becomes `la_storeptr_rel` AFTER
  mcode sync (§8.5) so any thread that observes the entry can execute the
  code. The bytecode patch (§8.4) is the real go-signal and is ordered
  after this store.
- `J->cur` trickery: during recording, `J->trace[curno]` points at the
  *stack-temporary* `&J->cur` (lj_trace.c:451). Under MT another thread
  could dereference it via vmevent/gc — forbid: registry entries for
  in-flight traces hold a tagged sentinel (`(GCRef)1`); every reader
  (`traceref`) treats ≤1 as NULL. GC marks the in-flight trace via the
  token holder's HS_SCAN_ROOTS ack (scan J->cur.* as roots while
  `jit_token==self`), replacing gc_traverse_curtrace (lj_gc.c atomic()).

## 8.4 Bytecode patching & side exits — no code patching, ever

### 8.4.1 bc_publish
`trace_stop` (lj_trace.c:499) today does `setbc_op(pc, …)` then
`setbc_d(pc, traceno)` — two narrow stores; a concurrent interpreter could
execute a torn JLOOP. Replace every bytecode patch (trace_stop, penalties
unpatching, flush restore, prof/hook ILOOP swaps in lj_dispatch.c) with:
```c
static LJ_AINLINE void bc_publish(BCIns *pc, BCIns ins)
{ la_store32_rel((uint32_t *)pc, ins); }
/* compose the full instruction first: BCINS_AD(JLOOP, slots, traceno) */
```
A racing thread executes either the complete old or complete new
instruction — both valid (JLOOP with a published trace, or the original
loop op). The ICACHE question doesn't arise: bytecode is data.

### 8.4.2 Exit tables replace lj_asm_patchexit
Today attaching a side trace rewrites the parent's machine code
(`lj_asm_patchexit`, called from trace_stop lj_trace.c:531) and flips
whole-area W^X (lj_mcode.c mcode_protect) — both are show-stoppers with
concurrent executors. New design:
```c
/* GCtrace gains: */ MCode **exittab;   /* nexits slots, allocated with T */
```
- Assembler change (lj_asm.c exit-stub emission, lj_emit_x86.h /
  lj_asm_x86.h `asm_exitstub_*`): instead of `jmp exitstub_group+exitno`,
  each guard's taken-branch lands on a per-exit thunk:
  `mov r11, [rip+disp_to_exittab_slot]` … `jmp r11` — or, denser
  (DECIDED): guards jump to a per-trace stub row `stub_i:
  mov ExitNoReg, i; mov r11,[T->exittab + i*8]; jmp r11` materialized at
  trace tail (the address `&T->exittab[i]` is a 64-bit constant at assembly
  time — T->exittab is allocated *before* asm starts, token-held).
- Initial value of every slot: the generic `->vm_exit_handler` trampoline
  (per-arch, exists today as exit-group target) which reads ExitNoReg and
  the trace number (from a per-trace constant register or the stub row’s
  trailing data word) and performs today's lj_trace_exit path.
- Attaching side trace = `la_storeptr_rel(&parent->exittab[exitno],
  child->mcode)` after child publication. Detach/flush = store the
  trampoline back. **Parent mcode never changes.**
- Cost: side-exit-taken path gains one indirect branch (memory-indirect
  jmp). Guards that never had a side trace go through the same row —
  today they jump to group stubs anyway; net new cost ≈ 1 load per taken
  exit. Exit-not-taken (the hot case) costs **zero**: the conditional jump
  target is the stub row exactly as it is the group stub today.
- `snap->count = SNAPCOUNT_DONE` and topslot bump (lj_trace.c:533–537):
  token-held plain stores; racy readers are the hotside counter (advisory).
- Trace stitching `traceref(J,J->exitno)->link = traceno`
  (lj_trace.c:551): `la_store16_rel` on the link field; consumed by
  lj_trace_exit dispatch decisions — stale read ⇒ one extra interpreter
  round-trip, harmless.
- Root/side chains (`pt->trace`, `T->nextroot/nextside`, lj_trace.c:519,
  544–548): token-held writes; GC reads under §5.7.4 rules (these are
  TraceNo1 indices, single 16-bit words — make them la_store16_rel; GC
  tolerates staleness because executing-trace roots come from TG.vmstate).

### 8.4.3 ExitNoReg / trampoline scratch
Per-arch: x64 uses the existing exit-stub register conventions
(group stubs push exitno today — keep the same register/stack slot so
->vm_exit_handler is unchanged except trace-number sourcing: store traceno
as a 4-byte trailer after the stub row and have the trampoline load it
PC-relative). Non-x64 stubs are out of scope for this pass.
`ExitTrampolines` in TG (03 §3.2) is only scratch spill space for the
handler — sized LJ_MAX_EXITSTUBGR-compatible; see lj_vmstruct notes.

## 8.5 mcode: allocation, W^X, cross-core publication

- Areas remain per-J (token-held cursor bumps, lj_mcode.c mcode_alloc
  logic + LJ_TARGET_JUMPRANGE constraint unchanged — all traces share the
  ±2 GB window so direct jumps keep working).
- W^X: `mcode_protect` flipping the whole area RW↔RX would fault
  concurrent executors. DECIDED: **dual mapping**. `memfd_create` one fd
  per area; map twice: RW view (assembler writes) and RX view (published
  addresses). `T->mcode` and every exittab/dispatch target use RX
  addresses; lj_asm writes via `mcode_rw(addr) = addr + (rw_base -
  rx_base)`. Delete the mcode_protect state machine (lj_mcode.c:233–260,
  396–430). Fallback for no-memfd kernels: plain RWX single mapping under
  `LUAJIT_INSECURE_MCODE` build flag.
- Publication order (token holder, end of trace_save):
  1. finish RW writes; 2. `__builtin___clear_cache(rx_lo, rx_hi)` (no-op
  on x86, kept as the portable publication hook replacing today's
  lj_mcode_sync call at lj_asm.c:2638); 3. `la_membarrier_synccore()` — `membarrier(
  MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE)` registered at VM init; this
  guarantees every core will execute the new instructions after it returns
  and orders the subsequent enabling stores against speculative fetch);
  4. `la_storeptr_rel(trace slot)`; 5. `bc_publish(...)` / exittab store.

## 8.6 GC interaction of running traces

- **Polls on trace**: emit IR `XPOLL` (new IR op, no result, GUARD-like) at
  every LOOP backedge and at FUNCF-entry of inlined frames ≥ depth K(=8).
  Codegen: `cmp dword [DISPATCH+TG_OFS(poll)],0; jnz exit_i` — i.e. a
  guard whose exit restores via the snapshot then lands in the interpreter,
  which immediately polls (07 §7.3). No special exit kind needed: reuse the
  nearest snapshot (force a snapshot at backedge — already the case).
  Loads of TG.poll must NOT be CSE'd/hoisted: mark XPOLL as having load
  effects (IRM ref to a new memory class) — fold rules: none.
- **Allocation on trace**: TNEW/TDUP/CNEW/SNEW already call into C or use
  inline alloc IR; route them to the TG bump (mirror of 07 §7.5) — the IR
  for inline alloc (lj_asm.c asm_snew/asm_tnew via lj_ir_call → actually
  allocations on trace call lj_tab_new etc. through IRCALL) stays C calls
  in v1 (DECIDED — inline-alloc IR is an M9 optimization); the C allocator
  polls/assists, satisfying pacing. Remove `lj_gc_step_jit` (lj_gc.c:764)
  and the `IR_GCSTEP`-ish machinery / `J->ircall` gc check emission —
  grep `gc_step_jit|GCSTEP` and excise with the legacy GC step path.
- **Barriers on trace** (§8.8.5 details): stores to heap (HSTORE/ASTORE/
  USTORE/FSTORE-with-gc-value/XSTORE-to-gcobj? XSTORE is cdata: exempt)
  emit the phase-gated mark sequence. The TGMARK load may be CSE'd *within
  a poll-free region*: model TGMARK as loaded via a special IR (`XBARFLG`)
  invalidated by XPOLL — gives loop-hoisted barrier checks reloaded each
  backedge: exactly right.
- **Executing-trace root**: vmstate already holds −traceno while in mcode
  (set in trace head, see lj_vm.h conventions / dasc vm_exit; verify file:
  `grep -n vmstate vm_x64.dasc`). TG.vmstate per 03 §3.3; GC reads it per
  05 §5.7.4. Traces additionally reachable via proto chains as today.

## 8.7 Flush protocol (jit.flush, blacklist-all, GC of traces)

Token-held: 1) `bc_publish` original instructions for every patched PC
(J->trace walk; the saved startins is in T->startins as today); 2) store
trampoline into every exittab slot; 3) handshake `HS_FLUSHJ|HS_EXIT_TRACES`
— after it, no thread is inside any trace (poll/exits forced) and none can
re-enter (bytecode restored before handshake); 4) defer_free mcode areas +
trace vector entries by one extra epoch (a thread could have been *about*
to jump… no: it would re-read bytecode after its ack; the handshake IS the
grace boundary — one epoch suffices, keep two for margin, constant
LJ_FLUSH_EPOCHS=2); 5) reset J fields as lj_trace_flushall does.
Individual trace GC (a dead proto's traces): same steps scoped to that
trace's PCs/parents, run by the leader between cycles with token.

## 8.8 Recorder/IR changes inventory

1. **HREFK & node addressing**: unchanged semantics — nodes never move
   within a gen (I-5). The recorded `hmask` guard now reads `t->hmask_c`
   (06 §6.2); add IRFL_TAB_HMASKC or repoint IRFL_TAB_HMASK's offset.
   Gen swap changes hmask_c ⇒ guard fails ⇒ exit ⇒ re-record against new
   gen. Same for IRFL_TAB_ASIZE→asize_c and IRFL_TAB_ARRAY/NODE which now
   load through arrayhdr/nodehdr +offsetof(slots/nodes): adjust the FLOAD
   lowering (lj_asm_x86.h asm_fload offsets table) — one extra indirection
   on first array/hash access per trace region; FLOADs of the hdr are
   CSE-able within poll regions (same XBARFLG-style invalidation class —
   DECIDED: table-hdr FLOADs join the poll-invalidated alias class so a
   resize between polls can't desync addr/asize pairs… simpler and fully
   sound: keep them ordinary FLOADs but make the *pair* (hdr ptr, cached
   size) come from the same hdr: guard on `AH->asize` loaded *through* the
   recorded AH pointer, never on asize_c inside the trace — asize_c is
   only for the pre-trace shape test. Then stale pairs are impossible.)
   IMPLEMENT the latter.
   Current implementation status: before the full `AHdr`/`NHdr` reshape, the
   legacy `GCtab` table-field `FLOAD`s for array/node/asize/hmask are emitted
   as fresh loads instead of being CSE'd under the old "no corresponding
   stores" assumption. This is an interim safety step for the release-published
   and retired legacy vectors, not a replacement for the original header-based
   FLOAD-indirection target above.
2. **TDUP/TNEW colo**: colo removed (06 §6.2) — recorder paths that
   special-case colocated arrays (`lj_record_tnew`, table.new fast func)
   simplify.
3. **ITERN/pairs recording**: gen-change ⇒ the standard "table modified"
   guard exit (06 §6.3.6); no new IR.
4. **CGET/CSET recording** (lj_record.c new cases): the cell ref in slot A
   is a TRef of type upval-object; emit `UREFC` on it then ULOAD/USTORE.
   The x64 groundwork can lower a direct cell `UREFC` from a slot TRef and
   handles raw-slot CGET/CSET fallback during pre-promotion execution.
   Owner-frame source and loaded v4 CGET/CSET can trace on x64 after
   separating local-cell opcodes from function-header/fast-function dispatch
   ranges. Source and loaded v4 child protos with parent-cell upvalues can
   trace through normal closed-upvalue UGET/USET recording after FNEW
   promotion. Original plan/WIP wording kept all loaded v4 cell protos
   `PROTO_NOJIT`; after audit, only self-captured local-function CNEW/CSET
   source protos and loaded v4 protos containing `BC_CNEW` stay `PROTO_NOJIT`
   until CNEW snapshot/FNEW recording behavior is implemented.
5. **Barrier IR**: extend the store lowerings: after computing the value
   ref, if `irt_isgcv(t)` emit `XBAR ref` (new IR, lowered to the §8.6
   guarded call/inline mark). Skip when value is a constant that the
   recorder can prove is a GC root (strings interned in proto constants
   are still markable cheaply — DECIDED: no skip; the runtime gate is
   cheap and correctness review stays trivial).
6. **String/intern fast funcs**: lj_ffrecord string.* unchanged (interning
   is in C). `__concat` buffers per-TG.
7. **vmevent/profiling on trace**: per-TG vmstate keeps lj_profile
   attribution working; lj_vmevent sends remain token-holder-only.

## 8.9 Files touched
lj_jit.h (token, tracev RCU, exittab field), lj_trace.c (token sites,
trace_stop/bc_publish/exittab, flush), lj_asm.c + lj_asm_x86.h/
lj_emit_x86.h (exit rows, dual-map writes, clear_cache),
lj_mcode.c (memfd dual map; delete protect FSM), lj_record.c (CGET/CSET,
XPOLL/XBAR emission), lj_ir.h (XPOLL/XBAR/alias classes), lj_opt_*.c
(fold/CSE rules for new IRs), lj_dispatch.c (record-mode per-TG dispatch,
bc_publish users), lj_gdbjit.c (RX addresses).
