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

Current M6 bridge: `global_State.jit_token` is a non-blocking recorder token
acquired with the current TG tid by hot root traces, hot side traces, and stitch
attempts. Record-mode dispatch callbacks only mutate `J` for the token holder,
and the x64 hot-loop/stitch paths pass `lua_State *` (plus the invoking trace
number for stitching) into C so `J->L`/stitch `J->exitno` are only written after
token acquisition. The x64/POSIX trace-exit path passes `L`/parent/exitno into
C and carries them as call-local snapshot-restore inputs; it publishes them to
shared `J` only after hot side exits acquire the recorder token. The trace
state machine releases the token whenever it returns to `LJ_TRACE_IDLE`, the
x64/POSIX unwound-trace error code is stored in the current TG instead of
shared `J->exitcode`, and token-busy hot side exits do not advance the retry
budget into `SNAPCOUNT_DONE`. Dispatch mode transitions now request
`HS_REDISPATCH` for already-attached TGs, so TG dispatch copies refresh through
their own safepoint ack. x64 VM entry paths now load `DISPATCH` from the
running `L->tg_hint` plus `offsetof(TGState, dispatch)`, and `TGPOLL` reads the
current TG's `poll` word. x64 VM slow paths load `global_State *` through
`TGState.gl` and `jit_State *` through `g->jitp` instead of fixed offsets from
`DISPATCH`. Fixed TG fields in the x64 emitter now use symbolic
`DISPATCH_TG(...)` offsets for `jit_base`, `cur_L`, `tmptv`, and `gl`; generic
`dispofs()` has been removed, with far non-MOV operands saving a scratch
register instead of clobbering `RID_DISPATCH`. `REF_NIL` GG-state FLOADs now
use absolute/RIP/global-address forms, and the x64 exit patcher recognizes the
resulting vmstate store patterns instead of scanning for `GG_OFS_TGDISP`.
Secondary TGs may now acquire the recorder token and enter `BC_JLOOP` mcode
through their own TG-local dispatch table. Record dispatch itself is localized
to the token holder's TG table instead of being exposed through the global
dispatch template. x64 LOOP-backedge traces now emit guarded `IR_XPOLL`,
lowered to a current-TG `poll` check that exits through the normal
trace-exit/VM safepoint path when a handshake is pending; hot-side recording is
skipped while that poll is pending.

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

Current M5 bridge: C-side `traceref()` acquire-loads trace slots, in-flight
slots use the pending sentinel instead of `&J->cur`, and final slots are
release-published before bytecode/exit/link go-signals. `TraceVec` now pairs
the acquired vector pointer with its size; growth allocate-copies and
release-publishes a new header, then retires the old header through the
safepoint epoch drain. The remaining original target here is the full trace
flush handshake/deferred mcode retirement and non-x64 exit-patcher removal.

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

Current M5 bridge: `bc_publish()`, `bc_publish_op()`, and `bc_publish_d()` live
in `lj_bc.h` and release-store complete 32-bit bytecode instructions. The
audited runtime patch sites in `lj_trace.c`, `lj_record.c`, and
`lj_dispatch.c` now use those helpers: trace stop, proto re-enable, unpatch,
blacklist/lazy-NOJIT ILOOP swaps, pending patch restore/unpatch, recorder TNEW
operand retuning, recorder JFUNC temporary unpatching, and `bc_cfunc_ext`
wrapping. Parser-local `setbc_*` fixups remain plain because they run before the
proto is published. The original target above remains broader: any newly found
runtime bytecode patch site must join this helper path, and side exits still
need the §8.4.2 exittab design to remove machine-code patching.

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

Current M5 bridge: shared trace-number links now use helper-wrapped 16-bit
release stores and acquire snapshots in GC/GC2, flush, bytecode writer, and
reflection paths. Current-trace assembler-private link reads remain plain.

Current M5 x64 exit bridge: `GCtrace` now owns `exittab` target slots plus
trace-local exit rows. x64 guards branch to rows that preserve registers, load
the writable target slot, and transfer to either the legacy interpreter exit
stub or a release-published child `mcode` pointer. `trace_stop()` publishes side
traces by `trace_exittarget_rel(parent, exitno, child->mcode)` after final trace
slot publication, so parent mcode is no longer rewritten on the supported x64
path. The older `lj_asm_patchexit()` implementations remain in non-x64 backends
for now; the original target remains to remove or replace those paths when those
architectures are brought into scope.

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
  Current M6 bridge: Linux/x64 VM init registers the private expedited
  sync-core membarrier when available, and `trace_stop()` calls
  `lj_mcode_sync_core()` after `lj_mcode_commit()` and before `trace_save()`
  performs the trace-slot release publication. The later bytecode, exittab,
  root/side-chain, and stitch-link release stores remain after `trace_save()`.
  Follow-up bridge: Linux/x64 `LJ_MT` secure builds now keep W^X without
  reopening published areas. `lj_mcode_reserve()` allocates a fresh
  unpublished area once the current area contains committed trace bytes, and
  `lj_mcode_commit()` flips only that fresh area RX before trace-slot,
  bytecode, exittab, and link publication. Reserve-time fresh allocation also
  checks `maxmcode`, since the legacy limit check only ran on area exhaustion.
  Final JIT teardown frees active mcode immediately with `lj_mcode_freeall()`
  instead of allocating new SMR retirement records after `lj_gc_freeall()`.
  This still does not implement the final memfd dual mapping; it replaces the
  temporary RWX bridge while preserving the original dual-map write-view
  target.

## 8.6 GC interaction of running traces

- **Polls on trace**: emit IR `XPOLL` (new IR op, no result, GUARD-like) at
  every LOOP backedge and at FUNCF-entry of inlined frames ≥ depth K(=8).
  Codegen: `cmp dword [DISPATCH+TG_OFS(poll)],0; jnz exit_i` — i.e. a
  guard whose exit restores via the snapshot then lands in the interpreter,
  which immediately polls (07 §7.3). No special exit kind needed: reuse the
  nearest snapshot (force a snapshot at backedge — already the case).
  Loads of TG.poll must NOT be CSE'd/hoisted: mark XPOLL as having load
  effects (IRM ref to a new memory class) — fold rules: none.
  Current M6 bridge: x86-64 emits guarded `IR_XPOLL` after `IR_LOOP` and at
  inlined Lua function entries once framedepth reaches 8, lowering both with
  `RID_DISPATCH` addressing the current TG-local dispatch table. This covers
  the LOOP-backedge and FUNCF-depth parts of the original requirement. The
  first TGMARK invalidation slice keeps `TBAR`/`OBAR` CSE inside poll-free
  regions and gates their x64 GC2 helper calls on the current TG's
  `mark_active` mirror, so a trace poll can enable the GC2 barrier before
  later heap stores. The broader XBAR/TGMARK alias model remains the follow-up
  target rather than being silently deleted. Existing FFI/raw-memory
  `IR_XBAR` alias limits now also treat `IR_XPOLL` as a poll-region boundary
  for `XLOAD` forwarding/CSE and `XSTORE` DSE; this advances the current XBAR
  surface without enabling table `ASTORE`/`HSTORE` tracing.
- **Allocation on trace**: TNEW/TDUP/CNEW/SNEW already call into C or use
  inline alloc IR; route them to the TG bump (mirror of 07 §7.5) — the IR
  for inline alloc (lj_asm.c asm_snew/asm_tnew via lj_ir_call → actually
  allocations on trace call lj_tab_new etc. through IRCALL) stays C calls
  in v1 (DECIDED — inline-alloc IR is an M9 optimization). Original report
  target retained: the C allocator polls/assists, satisfying pacing, then
  remove `lj_gc_step_jit` (lj_gc.c) and the `IR_GCSTEP`-ish machinery /
  `J->ircall` gc check emission — grep `gc_step_jit|GCSTEP` and excise with
  the legacy GC step path once replacement trace pacing is complete.
  Current M6 bridge: allocation calls still update legacy `g->gc.total` and
  now also accumulate positive allocation growth into `TG.local_total`, which
  flushes into `GC2State.alloc_since_trigger` at 32 KiB boundaries, safepoint
  ack, and TG detach. Starting a GC2/legacy mark cycle flushes TG-local totals
  and resets the trigger counter for the new cycle. `GC2State.trigger_bytes`,
  `hard_bytes`, `gcpause_pct`, and `assist_shift` are initialized and updated
  from `lua_gc` pause/stepmul controls. When flushed bytes pass
  `trigger_bytes` while GC2 is idle, the bridge requests a cycle by lowering
  the legacy `gc.threshold`, while still honoring `collectgarbage("stop")`;
  the request now also claims a nonblocking `GC2State.cycle_leader` token and
  records request/start telemetry so the eventual independent GC2 leader path
  has an owned request surface before the legacy threshold bridge is removed.
  The first bounded-assist bridge now has `lj_gc2_account_alloc()` call
  `lj_gc2_assist()` past `hard_bytes` during GC2 MARK/WEAK. Assists use
  `TGState.gc_assist` to prevent reentry, a nonblocking assist-owner token for
  the current global grey deque owner side, bounded active/published SSB
  conversion, and at most `1 << assist_shift` grey-object traversals. Keep
  `lj_gc_step_jit`/`IR_GCSTEP` guarded until independent concurrent cycle
  leadership, worker ownership, and trace allocation checks fully replace
  legacy pacing. Current staged deviation from the original removal target:
  `lj_gc_step_jit` now runs the same bounded GC2 assist bridge before legacy
  stepping, so trace-side GC checks can contribute GC2 mark work while the
  legacy `IR_GCSTEP` machinery remains present. x86-64 `asm_gc_check` now
  enters that helper when either the legacy GC threshold is reached or
  `GC2State.alloc_since_trigger > hard_bytes`; the helper gates legacy
  `lj_gc_step()` calls on the legacy threshold so GC2 hard-limit assists do
  not add extra legacy GC work. `GC2State.jit_hard_checks` and
  `tests/t-gc2-jit-hard-check.c` guard this staged path with a real x86-64
  allocation trace while preserving the original removal target above.
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

Current M5 bridge: x64 flush paths now release-reset every live `exittab` slot
back to the legacy interpreter exit stub before trace slots are cleared or a
root trace is unlinked. `lj_mcode_free()` detaches active mcode areas and moves
heap-side retirement records onto `J->retiredmcode`; dead `GCtrace` bodies link
themselves through `GCtrace.retired_next` and keep `exittab` storage until
retirement. Safepoint epoch drain physically unmaps/frees both after the
`LJ_FLUSH_EPOCHS=2` margin. This implements the data-retargeting part of step 2
and the reclamation half of step 4 for the supported x64 path. The full
public `jit.flush`/flushall path now calls `lj_trace_flushall_hs()`, which runs
`HS_EXIT_TRACES|HS_FLUSHJ` before the leader clears trace slots. This covers the
full-flush API part of the original protocol. Recorder-internal full-flush
recovery for trace-table exhaustion and mcode-allocation failure uses the same
`lj_trace_flushall_hs()` boundary. Original scoped bridge state: public
function/proto/trace flushes always published `HS_EXIT_TRACES` after their
existing root retarget/unpatch logic, even when the requested scope had no
trace object and did not disable a proto. Current scoped bridge:
`trace_flushroot()`, `lj_trace_flush()`, and `lj_trace_flushproto()` now report
scoped trace-exit publication work, and `jit.off(func)`/proto disabling counts
as boundary work even without existing traces. Public scoped flushes publish one
`HS_EXIT_TRACES` boundary only when that work count is nonzero. Current scoped
slot-retirement bridge: roots touched by scoped flush are tagged before the
handshake, dependent links and side-trace immediate parents are marked to
closure, and after the `HS_EXIT_TRACES` grace boundary explicitly marked
side-trace slots and side-trace slots rooted at flushed roots are cleared first,
then the root slots are cleared with `T->traceno = 0`. This makes trace numbers
reusable without leaving stale sides or cross-root tail links pointing at a
reusable trace number, and without letting the later GC sweep clear a reused
slot. The physical `GCtrace` body/exittab still reaches `J->retiredtraces`
through the existing sweep path, preserving the original bridge shape, but sweep
now keeps a finite scoped-retire epoch already stamped after the
`HS_EXIT_TRACES` boundary instead of replacing it with the later sweep epoch.
Full token ownership and per-root body retirement remain to finish the original
scoped-flush target.

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
   Current implementation note: `GCtab.hmask` is only a compatibility mirror
   after the landed `TabNodeHdr` slice, while C readers and regular x64
   dynamic `IR_HREF` lowering use the node header. The original JIT target
   above remains pending for the final generation-aware HREFK guard model and
   HSTORE/write codegen; current x64 HREFK lowering has an interim
   node-header slot-bounds guard before reading `node[slot].key`. Do not treat
   the mirror as the final correctness source.
   Current implementation status: before the full `AHdr`/`NHdr` reshape, the
   legacy `GCtab` table-field `FLOAD`s for array/node/asize/hmask are emitted
   as fresh loads instead of being CSE'd under the old "no corresponding
   stores" assumption. This is an interim safety step for the release-published
   and retired legacy vectors, not a replacement for the original header-based
   FLOAD-indirection target above. x64 regular `IR_HREF` lowering also loads
   the legacy node base before pairing it with a separately computed hmask
   index, so trace hash lookups do not combine a fresh hmask with an older node
   pointer during the current publish/retire phase. Constant-key `HREFK`
   recording snapshots the legacy node/hmask shape around `lj_tab_get()` and
   falls back to regular `HREF` if the shape changes while recording. Legacy
   table-slot stores are not recorded for now: the recorder raises the normal
   NYI-bytecode trace error before emitting `HSTORE` or `ASTORE` for indexed
   stores, and the M5 guardrail asserts both array and hash table-store loops
   stay untraced. The original plan kept traced array stores for barrier
   coverage, but the current M5 bridge demotes them until the final
   generation-aware trace write/barrier protocol can replace raw generated
   stores.
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
   `PROTO_NOJIT`; after audit and the first M6 CNEW/FNEW slice, self-captured
   local-function source protos and loaded v4 protos containing only the
   self-cell CNEW/FNEW/CSET shape no longer need that gate.
   Current M6 guard coverage requires IR dumps for owner numeric cells,
   GC-valued CSET with `OBAR`, loaded v4 CGET/CSET traces, source/loaded
   self-cell CNEW/FNEW helper traces, mixed raw-local sync-helper FNEW traces,
   mutable pre/post FNEW update loops after promotion at trace entry, and
   source/loaded first-promotion FNEW traces where the hot trace performs the
   first mutable raw-slot promotion with otherwise type-stable loop slots. The
   original broader local-cell target is preserved for remaining unguarded
   closure-construction combinations.
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
