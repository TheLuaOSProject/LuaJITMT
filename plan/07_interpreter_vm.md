# 07. Interpreter Changes (vm_x64.dasc)

Everything the hand-written interpreter must gain or change: safepoint
polls (§7.3), the new store barrier (§7.4), inline bump allocation (§7.5),
the three cell opcodes (§7.6), and the TG addressing migration already
specified in 03 §3.5. Work on vm_x64.dasc only in this pass. All dasc edits
land in M2 (TG addressing) and M4–M5 (polls/barrier/cells) per the plan in
12.

## 7.1 Inventory of dasc-level changes

| change | where | cost when inactive |
|---|---|---|
| TG addressing (dispositions A–F) | 03 §3.5 worklist (~45 sites) | 0 (same instr count for hot classes) |
| safepoint poll | backward-jump dispatch, call entry, alloc slow path, vm_returnc to C | 2 instrs, predicted not-taken |
| store barrier macro | replaces `barrierback` + adds array-store coverage | 2 instrs (load TG flag + jnz) |
| inline bump alloc | BC_TNEW/TDUP/CAT/FNEW/CNEW fast paths + vm_strnew? (no: strings allocate in C) | replaces gc.total check; net −1 instr |
| BC_CNEW/CGET/CSET handlers | new dispatch entries | n/a (new ops) |
| BC_JLOOP entry | reads whole BCIns atomically (already does: single 32-bit load) | 0 |
| ITERN gen-change detection | per 06 §6.3.6 | 1 cmp on iterator slow path |
| vm_safepoint stub | new global label | n/a |

## 7.2 Register & addressing conventions

No new pinned register. `DISPATCH` now points at `tg->dispatch` (03 §3.2);
all per-thread state is `[DISPATCH + DISPATCH_TG(field)]` with small
positive/negative displacements (TG layout was designed so every hot field
is within ±2 KB of dispatch[0] — int8/int32 disp encodings; verify with a
static assert in lj_tg.h: `TG_OFS(poll) < 0x7f0` etc.). The universe is one
load away: `mov GL, [DISPATCH+DISPATCH_TG(gl)]` — slow paths only.

dasc plumbing: add to the x64 header section (mirroring DISPATCH_GL at
vm_x64.dasc:326):
```
#define DISPATCH_TG(field) (TG_OFS(field))
|.define TGPOLL,  dword [DISPATCH+DISPATCH_TG(poll)]
|.define TGMARK,  byte  [DISPATCH+DISPATCH_TG(mark_active)]
```

## 7.3 Safepoint polls

### 7.3.1 Sites (DECIDED, exhaustive)
1. **Backward jumps.** The single dispatch point all loop ops share is the
   branch-taken path of BC_JMP/BC_LOOP/BC_FORL/BC_ITERL family. Insert in
   the common `|.macro ins_AJ`-adjacent jump path — concretely: in the
   `branchPC` expansion used by loop/jump handlers, after the PC update,
   gate on backward only where the encoding tells direction (RD < BCBIAS_J).
   Simplification (DECIDED): poll on *all* taken JMP-class branches, not
   just backward — saves the sign test; forward branches are rare in hot
   loops and the poll is 2 instructions:
   ```
   |  cmp TGPOLL, 0
   |  jnz ->vm_safepoint        // resumes by re-dispatching same PC
   ```
2. **Function entry.** In `vm_call_dispatch`/BC_FUNCF prologue after frame
   setup (covers deep recursion with no loops). One poll per call is too
   hot for FUNCF? Measured budget says: 2 predicted instrs ≈ 0.3ns; fib30
   gate in 13 will confirm; if it fails the gate, fallback F-3 (14 §14.3)
   moves the poll under the hotcall counter underflow path only.
3. **Allocation slow path** — C side (`alloc_slow`, 04 §4.4), no dasc work.
4. **Returns to C frames** (`vm_returnc`/`vm_leave_unw`): poll before
   leaving VM so long C-call-free script sections still ack promptly.
5. **JIT trace backedges/exits**: 08 §8.6, not dasc.

### 7.3.2 ->vm_safepoint stub
```
|->vm_safepoint:
|  // BASE/PC/KBASE live per dispatch convention; save L state
|  mov L:RB, SAVE_L
|  mov L:RB->base, BASE
|  mov SAVE_PC, PC                    // so GC stack scan sees frame
|  mov CARG1, L:RB
|  call extern lj_safepoint_ack       // 05 §5.4.2; may run HS actions
|  mov BASE, L:RB->base               // stack may NOT move (no realloc in
|  // ack path — invariant: ack never grows the Lua stack; assert in C)
|  ins_next                           // re-fetch+dispatch current PC
```
`lj_safepoint_ack` must be reentrancy-clean: no allocation that can recurse
into a poll (I-4 analog), no error throws. HS_REDISPATCH inside ack memcpys
the dispatch table the very PC we re-dispatch through — fine: ins_next
reloads from DISPATCH.

## 7.4 The store barrier macro

Replaces `barrierback` (vm_x64.dasc:378–383) and every dasc call into
`lj_gc_barrieruv`/`lj_gc_step`-coupled barrier logic. Semantics per 05
§5.14: *if marking is active and the stored value is collectable, mark it.*

```
|.macro wbarrier_tv, slotreg, tmp, tmp2   // value TValue in slotreg (u64)
|  cmp TGMARK, 0
|  jnz >9
|.label 8:
|  ...continue...
|9:                                       // cold section (|.code_sub)
|  mov tmp, slotreg
|  shr tmp, 47                            // itype
|  cmp tmp, LJ_TISGCV_SHIFTED             // is collectable? (precomputed
|  jb <8                                  //  constant; see note below)
|  // extract object pointer: mask off tag bits 47..63
|  mov tmp2, slotreg
|  and tmp2, GCMASK_PTR                   // (1<<47)-1
|  mov CARG1, DISPATCH-relative TG ptr    // lea CARG1,[DISPATCH+TG_OFS(...)]
|  mov CARG2, tmp2
|  call extern lj_gc2_markstore           // gc2_mark wrapper, C, no-throw
|  jmp <8
|.endmacro
```
Notes:
- Tag test: with GC64 NaN tagging, collectable itypes are LJ_TSTR(~4u)
  .. LJ_TUDATA(~12u) (lj_obj.h:264–272). The shifted-compare constant and
  exact branch form: copy the existing `checktp`-style idioms in the file;
  one unsigned range check suffices (itype−LJ_TUDATA ≤ LJ_TSTR−LJ_TUDATA).
- Call sites: every TSETV/TSETS/TSETB store (array AND hash — array stores
  are newly covered, 01 §1.3), USETV (cell store via CSET in v4; legacy
  USETx under !cells), TSETM, and the C-side equivalents already routed
  through lj_gc2_wbarrier (05 §5.14). The old "isblack(table)" pre-test
  disappears: the gate is the global-phase flag mirror TGMARK.
- The cold path clobbers per the file's scratch conventions; place bodies
  in the |.code_sub section like existing slow paths to keep hot lines
  unpolluted.
- Keys on hash insert are barriered in C (lj_tab_newkey path), not in dasc
  — dasc TSETS only stores values into *existing* keys; key-creating
  inserts always leave the fast path. Verify by reading BC_TSETS slow-path
  branch targets (`->vmeta_tsets`).

## 7.5 Inline bump allocation

Today BC_TNEW/TDUP check `gc.total>=gc.threshold` then call C
(vm_x64.dasc:3677–3711 region). Replace with TG bump (04 §4.4 fast path):

```
|.macro inline_alloc_traversable, ncells_imm, dstreg
|  mov RAd, [DISPATCH+DISPATCH_TG(alloc.bump_trav_cell)]
|  lea RCd, [RAd + ncells_imm]
|  cmp RCd, [DISPATCH+DISPATCH_TG(alloc.bump_trav_end)]
|  ja  ->vm_alloc_slow_##op              // C path: alloc_slow + retry op
|  mov [DISPATCH+DISPATCH_TG(alloc.bump_trav_cell)], RCd
|  mov dstreg, [DISPATCH+DISPATCH_TG(alloc.bump_trav_arena)]
|  // set block bit: bts [dstreg + offsetof(GCArena,block)], RAd  (owner-
|  // exclusive arena ⇒ plain bts, no lock prefix)
|  bts qword [dstreg+GCA_BLOCK_OFS], RA
|  // alloc-black: cold-flag test, lock bts into mark bitmap if set
|  cmp byte [DISPATCH+DISPATCH_TG(alloc.alloc_black)], 0
|  jnz >7    // cold: lock bts mark bitmap, then continue
|  shl RA, 4
|  lea dstreg, [dstreg + RA]             // object pointer
|.endmacro
```
Apply to: BC_TNEW (GCtab, 4 cells + array colo? colo removed by 06
§6.2 ⇒ plain 4-cell GCtab + separate AHdr alloc in C when asize>0 — keep
TNEW fast path only for the 0/0 template case, else C), BC_TDUP (C; it
copies), BC_FNEW (C already), BC_CNEW (§7.6), BC_CAT result string (no —
strings intern in C, keep call). Accounting: `add qword
[DISPATCH+DISPATCH_TG(local_total)], size` folded into the macro.

The old `lj_gc_check` sites (dasc:1186–87 et al) are deleted; pacing rides
the alloc slow path + safepoint (05 §5.11).

## 7.6 New opcodes (numbering in 10 §10.2)

```
|case BC_CNEW:                         // A = dst slot
|  inline_alloc_traversable 2, TAB:RB  // GCupval = 32B = 2 cells
|  // init header: gct=~LJ_TUPVAL, gcflags=0, closed=1, immutable=0
|  mov word [RB+offsetof(GCupval,gcflags)], LJ_TUPVAL_HDR16
|  mov byte [RB+offsetof(GCupval,closed)], 1
|  mov qword [RB+offsetof(GCupval,tv)], LJ_TNIL_U64
|  lea RA, [RB+offsetof(GCupval,tv)]
|  mov [RB+offsetof(GCupval,v)], RA    // v = &tv
|  // store tagged ref into slot A
|  lea RA, [BASE+RA*8] ... settp LJ_TUPVAL ... mov [slot], RB_tagged
|  ins_next
|
|case BC_CGET:                         // A = dst, D = cell slot
|  mov RB, [BASE+RD*8]                 // tagged uv ref
|  cleartp UPVAL:RB
|  mov RB, UPVAL:RB->v                 // == &uv->tv always (06 §6.4.1)
|  mov RB, [RB]                        // the value (single 8B load: I-1)
|  mov [BASE+RA*8], RB
|  ins_next
|
|case BC_CSET:                         // A = cell slot, D = src slot
|  mov RB, [BASE+RA*8]
|  cleartp UPVAL:RB
|  mov RC, [BASE+RD*8]                 // value
|  wbarrier_tv RC, RA, ...             // 05 I-2
|  mov RA, UPVAL:RB->v
|  mov [RA], RC                        // single 8B store: I-1
|  ins_next
```
UGET/USETV/USETS/USETN/USETP stay for ordinary upvalues in current bytecode.
The lockless cell model adds CGET/CSET for cell slots instead of loading old
dump compatibility through UGET/USETx.

## 7.7 FNEW, UCLO, hot counters, cur_L/jit_base touchpoints
- BC_FNEW handler: unchanged dasc (calls lj_func_newL_gc); the C side
  copies cell refs per 06 §6.4.2.
- BC_UCLO: under v4 no closing UCLO A != 0 is emitted for source cells.
  Stock-compatible v2/v3 dumps still load, but old-version dumps containing
  lockless-only cell opcodes are rejected and legacy-loaded functions are not
  re-dumped as current chunks.
- hotloop/hotcall (dasc:332–345): untouched (TG keeps GG_DISP2HOT, 03
  §3.2-A). HotCount races across threads don't exist — counters are
  per-TG now, which also fixes today's cross-coroutine pollution.
- cur_L/jit_base sites: disposition B of 03 §3.5 — same instruction count.

## 7.8 Non-x64 ports
Out of scope for this implementation pass. Do not spend milestone time on
non-x86-64 dasc work until the x86-64 Linux runtime is green.

## 7.9 Audit checklist for this document's changes
- [x] `grep -c "vm_safepoint" vm_x64.dasc` ≥ 4 sites + stub
- [x] no remaining `DISPATCH_GL(gc\.` after M2
- [x] x64 `barrierback` macro deleted; TSETV/TSETS/TSETB/TSETM and closed
      USETx/CSET stores route through helper-backed publication barriers
- [x] BC_TNEW slow-path label still reachable for asize>0 templates
- [x] interp-only build (`-joff`) passes the stock suite

Current guard: `m3_vm_safepoint` now asserts the migrated x64 VM source
invariants before running `t-vm-safepoint`: at least five `vm_safepoint`
references, no x64 `DISPATCH_GL(gc.*)` loads, no x64 inline `barrierback`, and
both the empty-table `lj_tab_new0` and non-empty `lj_tab_new` `BC_TNEW` paths
remain present.

Current stock guard: `m3_interp_stock_joff` builds the default x64 VM and runs
the vendored stock suite as `luajit -joff test.lua --quiet`; the current pass
reports `386 passed`.

Current store-publication guard: `m5_x64_vm_store_publication` asserts the x64
VM cannot reintroduce legacy inline `barrierback`/`lj_gc_barrieruv` paths,
keeps `TSETV`/`TSETB`/`TSETR` array stores on
`lj_tab_storetv_forvm_array()`, keeps `TSETM` range stores on
`lj_tab_storetvn_forvm_array()`, keeps `TSETS` on the C fallback instead of a
direct hash-slot store, and keeps closed `CSET`/`USETx` paths on
`lj_func_storeuv_*_pub()` release-copy helpers.

Current x64 bridge note: the base-library `setmetatable` fast path now
publishes the table -> metatable edge through `lj_gc2_barrier_obj_pair()` before
the legacy black-table repair. The x64 `TSETV`, `TSETB`, `TSETR`, and `TSETM`
fast array/range stores now publish slots first and route post-store checks
through VM helpers that combine parent-aware GC2 barriers with legacy
incremental black-table repair. This helper-backed x64 path supersedes the
original inline `wbarrier_tv` sketch in favor of a more conservative,
C-auditable safety surface; x64 closed `USETx`/`CSET` stores release-copy through
`lj_func_storeuv_*_pub()` helpers, and raw/open cell stores remain stack-local
writes. The old x64 `vm_gc2_barriertab` helper
label has no remaining VM branch users and is retired; the JIT C-call
`lj_gc2_barrier_tab_g` path remains separate. The interpreter allocation slow
path now also runs the GC2 hard-threshold assist from `lj_gc_step_fixtop()` once
the current legacy VM threshold check branches there, with
`GC2State.interp_hard_checks` telemetry. `lj_gc_step_fixtop()` now also splits
GC2-hard and legacy-threshold work. x64 `BC_TNEW`/`BC_TDUP` and the
fast-function `ffgccheck` path call the C-owned `lj_gc_should_step_vm()`
predicate before entering the existing step helpers, so legacy total/threshold
and GC2 hard-threshold reads no longer live in VM assembly. C-side allocation
checks use the same `lj_gc_should_step()` predicate and split helpers.
`lj_tab_new()` now constructs `GCtab` bodies from unlinked raw GC storage,
initializes the empty/colocated table shape, nil-clears new array slots, then
CAS-publishes the table root with `lj_gc_linkobj()`. This removes the
publish-before-body-init blocker for future `BC_TNEW` inline allocation; the
empty-table x64 `BC_TNEW` case now branches to the one-argument
`lj_tab_new0()` helper. Fresh table arrays use the same publication order as
resize (`acap` mirror, release array pointer, release `asize` mirror), so a
concurrent reader does not pair a new size with an unpublished array pointer.
The remaining x64 fast path still needs inline legacy color setup, root-list
publication, and GC2 allocation accounting before it can bypass the C helper
entirely.
