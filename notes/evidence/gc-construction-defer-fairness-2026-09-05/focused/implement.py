from pathlib import Path
import json,hashlib,difflib
P=Path('/tmp/lj-reclaim-fair-pass-20260905-kw8kfdam');R=P/'candidate/src'
def ch(name,a,b):
 q=R/name;s=q.read_text();assert s.count(a)==1,(name,a,s.count(a));q.write_text(s.replace(a,b))
ch('lj_obj.h','  MSize grey_capacity;\t/* Allocated grey deque slots. */\n  uint64_t grey_top;', '  MSize grey_capacity;\t/* Allocated grey deque slots. */\n  uint32_t sweep_owner_next_tid;  /* Worker-owned sweep scheduling hint. */\n  uint64_t grey_top;')
ch('lj_obj.h','static LJ_AINLINE TGState *gc2_tg_list_acq(global_State *g)', '''/* The worker token serializes this scalar hint. It never pins a TG body. */
static LJ_AINLINE uint32_t gc2_sweep_owner_next_tid_acq(global_State *g)
{
  return la_load32_acq(&g->gc2.sweep_owner_next_tid);
}

static LJ_AINLINE void gc2_sweep_owner_next_tid_store_rlx(global_State *g,
                                                       uint32_t tid)
{
  la_store32_rlx(&g->gc2.sweep_owner_next_tid, tid);
}

static LJ_AINLINE TGState *gc2_tg_list_acq(global_State *g)''')
ch('lj_gc2.c','  gc2_sweep_grace_needed_rel(g, 0);\n  gc2_sweep_to_idle_store_rlx(g, 0);','  gc2_sweep_grace_needed_rel(g, 0);\n  gc2_sweep_owner_next_tid_store_rlx(g, 0);\n  gc2_sweep_to_idle_store_rlx(g, 0);')
ch('lj_gc2.c','static uint32_t lj_gc2_sweep_owner_progress(global_State *g, TGState *tg,\n\t\t\t\t\t     uint32_t limit,\n\t\t\t\t\t     int *finishedp)','static uint32_t lj_gc2_sweep_owner_progress(global_State *g, TGState *tg,\n\t\t\t\t\t     uint32_t limit,\n\t\t\t\t\t     int *finishedp,\n\t\t\t\t\t     int *root_owner_blockedp)')
ch('lj_gc2.c','  if (finishedp)\n    *finishedp = 0;\n  if (!g || !tg || limit == 0 ||','  if (finishedp)\n    *finishedp = 0;\n  if (root_owner_blockedp)\n    *root_owner_blockedp = 0;\n  if (!g || !tg || limit == 0 ||')
ch('lj_gc2.c','      if (root_owner_blocked)\n\tgc2_quantum_defer(g);','      if (root_owner_blocked && root_owner_blockedp)\n\t*root_owner_blockedp = 1;')
ch('lj_gc2.c','  int claimed = 0;\n  int gate_owned = 0;\n  if (!g || !tg || limit == 0)','  int claimed = 0;\n  int gate_owned = 0;\n  int root_owner_blocked = 0;\n  if (!g || !tg || limit == 0)')
ch('lj_gc2.c','    n = lj_gc2_sweep_owner_progress(g, tg, limit, NULL);','    n = lj_gc2_sweep_owner_progress(\n      g, tg, limit, NULL, &root_owner_blocked);\n    if (root_owner_blocked)\n      gc2_quantum_defer(g);')
ch('lj_gc2.c','static uint32_t gc2_worker_sweep_progress(global_State *g, uint32_t limit)\n{\n  TGState *tg;\n  uint32_t n = 0;','static uint32_t gc2_worker_sweep_progress(global_State *g, uint32_t limit)\n{\n  TGState *tg, *head, *start;\n  uint32_t n = 0, next_tid;\n  int root_owner_blocked = 0;')
a='''  for (tg = gc2_tg_list_acq(g);
       tg != NULL && n < limit;
       tg = lj_tg_next_acq(tg)) {
    uint8_t flags = lj_tg_flags_acq(tg);
    int finished = 0;
    if ((flags & (TGF_DEAD|TGF_ARENA_INTERNAL)) != TGF_ARENA_INTERNAL)
      continue;
    n += lj_gc2_sweep_owner_progress(g, tg, limit - n, &finished);
    if (finished || gc2_deferred_epoch_acq(g) != defer0)
      break;
  }
  return n;  /* 05 section 5.6.3 worker-owned sweep bridge. */'''
b='''  /* worker_active excludes physical TG unlink/reclaim during this pass.
  ** Attach only prepends; capture that head once so new owners join the next
  ** invocation. Resolve the scalar hint through this list, never through a
  ** retained TG pointer. The existing self-next attach repair is a tail. */
  head = start = gc2_tg_list_acq(g);
  next_tid = gc2_sweep_owner_next_tid_acq(g);
  if (next_tid != 0) {
    for (tg = head; tg != NULL;) {
      TGState *next;
      if (lj_tg_tid_acq(tg) == next_tid) {
        start = tg;
        break;
      }
      next = lj_tg_next_acq(tg);
      if (next == tg)
        break;
      tg = next;
    }
  }
  tg = start;
  while (tg != NULL && n < limit) {
    TGState *next = lj_tg_next_acq(tg);
    uint8_t flags = lj_tg_flags_acq(tg);
    int finished = 0, owner_blocked = 0;
    if (next == NULL || next == tg)
      next = head;
    if ((flags & (TGF_DEAD|TGF_ARENA_INTERNAL)) == TGF_ARENA_INTERNAL) {
      n += lj_gc2_sweep_owner_progress(
        g, tg, limit - n, &finished, &owner_blocked);
      root_owner_blocked |= owner_blocked;
    }
    /* Preserve the work quota and one-finished-arena boundary, but resume at
    ** the next owner after either boundary. A persistent EOF retry must not
    ** reclaim the front of every subsequent invocation. */
    gc2_sweep_owner_next_tid_store_rlx(g, next ? lj_tg_tid_acq(next) : 0);
    if (finished || gc2_deferred_epoch_acq(g) != defer0)
      break;
    tg = next;
    if (tg == start)
      break;  /* At most one pass of the captured list, including zero work. */
  }
  /* All physical writer cleanup preceded the local refusal. Give independent
  ** owners their bounded turns, then stop higher-level immediate retry loops.
  ** Another consumer's event already supplies the same invocation boundary. */
  if (root_owner_blocked && gc2_deferred_epoch_acq(g) == defer0)
    gc2_quantum_defer(g);
  return n;  /* 05 section 5.6.3 worker-owned sweep bridge. */'''
ch('lj_gc2.c',a,b)
record=json.loads((P/'source-identity.json').read_text());prev=Path(record['starting_source']);base=Path('/tmp/lj-gc-auto-stop-overlap-20260905-y4h4cc8a/controlextrahelpers')
for label,source in [('fair-delta',prev),('candidate',base)]:
 patch='';changed=[]
 for rel in record['inputs']:
  before=(source/rel).read_bytes();after=(P/'candidate'/rel).read_bytes()
  if before!=after:
   changed.append(rel)
   patch+=''.join(difflib.unified_diff(before.decode().splitlines(keepends=True),after.decode().splitlines(keepends=True),fromfile='a/'+rel,tofile='b/'+rel))
 (P/(label+'.patch')).write_text(patch)
 print(label,changed,hashlib.sha256(patch.encode()).hexdigest())
