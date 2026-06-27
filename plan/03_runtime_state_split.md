# 03. Runtime State Split: global_State, GG_State, and the per-thread TG block

Today one `GG_State` block holds everything and the asm `DISPATCH` register
points into it (lj_dispatch.h:89–119):

```
GG_State { lua_State L; global_State g; jit_State J;
           HotCount hotcount[64]; ASMFunction dispatch[GG_LEN_DISP];
           BCIns bcff[GG_NUM_ASMFF]; }
```
All asm access to globals is `[DISPATCH + GG_DISP2G + offsetof(global_State,
field)]` via the `DISPATCH_GL()` macro (vm_x64.dasc:326), hotcounts via
`GG_DISP2HOT` (lj_dispatch.h:122), J via `GG_DISP2J`. With multiple OS
threads, three different lifetimes are tangled in there and must be split.

## 3.1 The three lifetimes

A. **Universe-global, shared** — `global_State g`, `jit_State J` (under the
   recorder token), `bcff`, dispatch *templates*.
B. **Per OS thread** — everything a thread touches per-bytecode or
   per-allocation: dispatch copy, hotcounts, safepoint word, allocator,
   grey SSB, cur_L, jit_base, tmp buffers, prng.
C. **Per lua_State** — unchanged (stack, frames, status, openupval legacy
   list, env).

## 3.2 The TG block (new struct, src/lj_tg.h)

`DISPATCH` now points at `tg->dispatch` of the running thread's TG block.
Layout is chosen so the *existing* asm offset macros keep working where the
field stayed adjacent to dispatch, and per-thread fields get new small
constant offsets:

```c
typedef struct TGState {
  /* ---- negative offsets from dispatch[]: mirrors GG layout ---- */
  /* (order matters; keep hotcount immediately before dispatch so   */
  /*  GG_DISP2HOT == TG_DISP2HOT and the hotloop/hotcall macros in  */
  /*  vm_*.dasc are unchanged: lj_dispatch.h:125–128, dasc:332–345) */
  HotCount hotcount[HOTCOUNT_SIZE];

  /* ---- dispatch tables: per-thread copy ---- */
  ASMFunction dispatch[GG_LEN_DISP];

  /* ---- positive offsets from dispatch[]: hot per-thread state ---- */
  uint32_t poll;          /* 0 = run; nonzero = handshake requested      */
  uint32_t mark_active;   /* mirror of g->gc2.phase==P_MARK (store barrier)*/
  global_State *gl;       /* the universe                                 */
  lua_State *cur_L;       /* currently executing lua_State (moved from g) */
  TValue *jit_base;       /* moved from g->jit_base                       */
  uint8_t  in_native;     /* native-state flag (05 §5.4.3, 11 §11.5)      */
  uint8_t  gc_assist;     /* allocator should run mark assist (05 §5.11)  */
  uint8_t  hookmask_th;   /* reserved (v1 hooks are global; 03 §3.6)      */
  uint8_t  tg_flags;
  uint32_t reqmask;       /* handshake action bits (05 §5.4.2)            */
  uint64_t hs_epoch_ack;  /* last handshake epoch this thread acked       */
  /* allocator (04 §4.6) */
  TGAlloc  alloc;
  /* grey sequential store buffer (05 §5.6) */
  GCRef   *ssb_next, *ssb_end; GCRef *ssb_base;
  /* per-thread scratch moved out of global_State */
  SBuf     tmpbuf;        /* was g->tmpbuf  (lj_obj.h:646)                */
  TValue   tmptv, tmptv2; /* was g->tmptv*  (lj_obj.h:647)                */
  PRNGState prng;         /* was g->prng    (lj_obj.h:662)                */
  /* roots & bookkeeping */
  lua_State *thread_L;    /* the lua_State this OS thread was spawned with*/
  struct TGState *next_tg;/* lock-free list of all TGs (05 §5.4.1)        */
  uint64_t local_total;   /* bytes allocated since last flush (04 §4.8)   */
  uint64_t stack_dirty_epoch; /* root rescan optimization (05 §5.7.3)     */
  ExitTrampolines *exittr;/* per-thread trace-exit scratch (08 §8.4)      */
} TGState;
#define TG_DISP2HOT  (-(int)(HOTCOUNT_SIZE*sizeof(HotCount)))
#define TG_OFS(f)    ((int)(offsetof(TGState,f) - offsetof(TGState,dispatch)))
#define DISPATCH_TG(f) TG_OFS(f)         /* for dasc */
```
Primary layout: `TGState` is embedded in GG_State for the main OS thread and
`tg->gl` points at the adjacent `global_State`. GG_State becomes
`{ lua_State mainL; global_State g; jit_State J; TGState main_tg; BCIns bcff[]; }`;
`GG_DISP2J`/`GG_DISP2G` survive only in C via pointers (see §3.4).

Size note: GG_LEN_DISP ≈ 2*BC__MAX+ff entries ⇒ dispatch copy ≈ 16 KB; a TG
block is ~20 KB, allocated page-aligned from the OS (not the GC heap),
cache-line padded between `poll` and write-mostly allocator fields.

## 3.3 Disposition of every global_State field (lj_obj.h:634–664)

| field | disposition |
|---|---|
| allocf/allocd | DELETED (allocator replaced; lua_newstate's allocf is accepted but only consulted for the arena source hook, 04 §4.9) |
| gc (GCState) | replaced by `GC2State gc2` (05 §5.3); legacy collector retained only as a temporary reference/debug aid where explicitly called out |
| strempty/stremptyz | stays in g (immutable after init; asm sites read via cached TG? not needed — only C touches it) |
| hookmask, hookcount, hookcstart, hookf | stay in g; global hooks v1 (§3.6) |
| dispatchmode | stays in g; guarded by recorder token; updates fan out (§3.5) |
| vmevmask, wrapf, panic, bc_cfunc_int/ext | stay in g (init-time/rare) |
| str (StrInternState) | reworked: `tab+mask` become one RCU `StrTabHdr*` (06 §6.5); `num` atomic; `id/idreseed` atomic; `seed` immutable |
| vmstate | becomes per-thread: TG.vmstate (profiler reads per-TG) |
| mainthref, vmthref, registrytv, gcroot[], nilnode | stay in g; gcroot writes via la_*; nilnode is read-only fallback (val must stay nil — a racy write into nilnode would be catastrophic; 06 §6.2.7 removes the nilnode-as-freetop trick) |
| tmpbuf, tmptv, tmptv2 | → TG (every C user gets `tg` via `G2TG(L)`; grep `g->tmpbuf\|G(L)->tmpbuf` ≈ 40 sites: lj_str.c, lj_strfmt*.c, lj_buf.c, lib_string.c, lj_cconv.c, lj_bcwrite.c, lj_debug.c …; mechanical sed + compile) |
| uvhead | DELETED (no global open-uv list; legacy per-L lists remain on L->openupval during migration; GC walks per-thread, 05 §5.7.4) |
| cur_L, jit_base | → TG (asm sites listed in §3.5) |
| ctype_state | stays (11) |
| prng | → TG (math.random becomes per-thread-seeded; doc note 09 §9.8) |

`lua_State` additions: `TGState *tg_hint` (set while running; for C API
re-entry), `uint32_t thr_owner` (06 §6.7), `uint64_t scan_epoch`.

New accessors: `#define TG(L) ((TGState *)(L)->tg_hint)` valid only while
running on its owner; C entry points re-derive via thread-local
`lj_tls_tg` (one `__thread TGState*` — TLS is allowed: it is not a lock).

## 3.4 C-side mechanical migration

1) Add `lj_tg.h`; include from lj_obj.h. 2) Replace `G2GG/L2GG/J2GG` users:
J is reached as `G2J(g) := g->jitp` — add `jit_State *jitp;` to g pointing
at the GG-embedded J (keeps lj_trace.c etc. compiling with one sed:
`L2J(L)` → `G(L)->jitp`). 3) `lj_dispatch_init` builds the *template*
dispatch in g (`g->disp_tmpl[GG_LEN_DISP]`), and `lj_tg_attach()` memcpys
template→TG. 4) Every `setsbufL`-style tmpbuf use: pass tg.

## 3.5 dasc migration (vm_x64.dasc)

Generate the worklist:
`grep -n "DISPATCH_GL(\|GG_DISP2G\|GG_DISP2J\|GG_DISP2HOT" src/vm_x64.dasc`
(45 hits at the pinned commit). Dispositions:

A. `GG_DISP2HOT` (hotloop/hotcall macros, dasc:332–345 and vm_hotloop/
   vm_hotcall): UNCHANGED — TG keeps hotcounts at the same relative offset.
B. `DISPATCH_GL(cur_L)` (dasc:595,636,674,1584,2438,4794) and
   `DISPATCH_GL(jit_base)` (2439,2448,2496,4633): become
   `[DISPATCH+DISPATCH_TG(cur_L)]` / `(jit_base)` — same instruction count.
C. `DISPATCH_GL(gc.total)/gc.threshold` (1186–87, 3677–78, 3709–11; the
   `lj_gc_check` fast path before string/table allocs): replaced by the new
   allocation poll: `cmp dword [DISPATCH+DISPATCH_TG(poll)],0; jnz ->vm_safepoint`
   plus the bump-allocator inline (07 §7.5). gc.total no longer exists.
D. `barrierback` macro (dasc:378–383) and all `lj_gc_barrieruv`/barrier
   call sites: replaced wholesale by the new store-barrier macro (07 §7.4).
E. `GG_DISP2G` used to materialize `g` for C calls (dasc:3583,3611 …):
   becomes `mov GL:CARG1, [DISPATCH+DISPATCH_TG(gl)]` — one extra load,
   only on slow paths into C.
F. `DISPATCH_GL(hookmask)` & friends (dispatch entries for hooks/records):
   keep reading g via the TG.gl indirection (slow paths only).
Everything else in the hit list falls into one of A–F; classify each x86-64
line in the commit message.

## 3.6 Dispatch table replication & hooks

`lj_dispatch_update(g)` (lj_dispatch.c) flips handlers for hooks/profiler/
recording. In the lockless runtime it (a) takes the recorder token (08 §8.2), (b)
rewrites the template, (c) issues handshake `HS_REDISPATCH`: each thread
memcpys template→its TG dispatch at its next poll (16 KB copy; rare).
Consequence (documented limitation L3): `debug.sethook`/profiler apply
universe-wide and take effect at each thread's next safepoint. Per-thread
hooks are a v2 item with `hookmask_th` reserved.

## 3.7 lua_State / coroutine ownership recap

`L->thr_owner`: 0=free, tid=running on that OS thread, GCSCAN=claimed by a
marker. `coroutine.resume` does CAS 0→tid (error "coroutine already running
on another thread" on tid≠self mismatch; "claimed by GC" waits through a
no-`lua_State` native sleep slice. The scanner claim remains bounded by stack
size, but the safety-first implementation avoids burning CPU if the scanner is
preempted while holding `LJ_THREAD_GCSCAN`; justified in 05 §5.7.2).
`lua_newthread`/coroutine.create unchanged otherwise.

## 3.8 Order of work

This split is M1/M2 of the plan (12): land TGState with single-thread
semantics first (stock tests green), then the dasc migration under a temporary
assert that only one thread exists, then flip pieces live milestone by
milestone.
