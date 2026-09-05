from pathlib import Path
import tempfile,hashlib,json,shutil,subprocess,difflib
P=Path(tempfile.mkdtemp(prefix='lj-reclaim-owner-defer-20260905-'))
Path('/tmp/lj-reclaim-owner-defer-current').write_text(str(P)+'\n')
S=Path('/tmp/lj-gc-auto-stop-overlap-20260905-y4h4cc8a/controlextrahelpers')
old=Path('/tmp/lj-func-construction-timeout-20260905-8htcwnmd')
files=json.loads((old/'source-identity.json').read_text())['inputs']
def sha(q):return hashlib.sha256(q.read_bytes()).hexdigest()
inputs={}
for rel in files:
 q=S/rel;inputs[rel]=sha(q)
 target=P/'candidate'/rel;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(q,target)
(P/'source-identity.json').write_text(json.dumps({'base':'597b8705208957ade8465416da30976ab9b52195','source_tree':str(S),'inputs':inputs},indent=2)+'\n')
(P/'prior').mkdir()
for name in ['HANDOFF.md','PROPOSAL.md','manifest.json']:
 shutil.copyfile(old/name,P/'prior'/name)
R=P/'candidate/src'
def change(name,a,b):
 q=R/name;s=q.read_text();assert s.count(a)==1,(name,a,s.count(a));q.write_text(s.replace(a,b))
change('lj_gc.h','LJ_FUNC uint32_t lj_gc_reclaim_gc2_arena(global_State *g, GCArena *a,\n\t\t\t\t\t uint32_t limit, int *donep);','LJ_FUNC uint32_t lj_gc_reclaim_gc2_arena(global_State *g, GCArena *a,\n\t\t\t\t\t uint32_t limit, int *donep);\n/* Report an exact intrusive owner without changing its reclaim retry state. */\nLJ_FUNC uint32_t lj_gc_reclaim_gc2_arena_ex(global_State *g, GCArena *a,\n\t\t\t\t\t    uint32_t limit, int *donep,\n\t\t\t\t\t    int *root_owner_blockedp);')
change('lj_gc.c','uint32_t lj_gc_reclaim_gc2_arena(global_State *g, GCArena *a,\n\t\t\t\t  uint32_t limit, int *donep)','uint32_t lj_gc_reclaim_gc2_arena_ex(global_State *g, GCArena *a,\n\t\t\t\t     uint32_t limit, int *donep,\n\t\t\t\t     int *root_owner_blockedp)')
change('lj_gc.c','  int pending = 0;\n  if (donep)\n    *donep = 0;\n  if (!g || !a || limit == 0 ||','  int pending = 0;\n  if (donep)\n    *donep = 0;\n  if (root_owner_blockedp)\n    *root_owner_blockedp = 0;\n  if (!g || !a || limit == 0 ||')
change('lj_gc.c','      ** reuse veto. Reclaim never waits for it or inspects a mutable header. */\n      pending = 1;','      ** reuse veto. Reclaim never waits for it or inspects a mutable header. */\n      if (root_owner_blockedp)\n\t*root_owner_blockedp = 1;\n      pending = 1;')
change('lj_gc.c','static void gc2_huge_sweep_reader_drop(LJHugeReader *reader, int *pendingp)','uint32_t lj_gc_reclaim_gc2_arena(global_State *g, GCArena *a,\n\t\t\t\t  uint32_t limit, int *donep)\n{\n  return lj_gc_reclaim_gc2_arena_ex(g, a, limit, donep, NULL);\n}\n\nstatic void gc2_huge_sweep_reader_drop(LJHugeReader *reader, int *pendingp)')
change('lj_gc.c','  while (running && step_limit-- != 0) {\n    uint32_t phase = gc2_phase_acq(g);','  while (running && step_limit-- != 0) {\n    uint64_t defer0 = gc2_deferred_epoch_acq(g);\n    uint32_t phase = gc2_phase_acq(g);')
change('lj_gc.c','    done = lj_gc2_step_explicit(L, 1);\n    if (gc2_phase_acq(g) == LJ_GC2_IDLE)','    done = lj_gc2_step_explicit(L, 1);\n    /* A retained owner ends this automatic batch as well as the inner step. */\n    if (gc2_deferred_epoch_acq(g) != defer0)\n      break;\n    if (gc2_phase_acq(g) == LJ_GC2_IDLE)')
change('lj_gc2.c','    if (qa) {\n      int done = 0;\n      int finished_arena = 0;','    if (qa) {\n      int done = 0;\n      int finished_arena = 0;\n      int root_owner_blocked = 0;')
change('lj_gc2.c','\tuint32_t reclaimed_step = lj_gc_reclaim_gc2_arena(g, qa, 64u, &done);','\tuint32_t reclaimed_step = lj_gc_reclaim_gc2_arena_ex(\n\t  g, qa, 64u, &done, &root_owner_blocked);')
change('lj_gc2.c','      gc2_sweep_reclaim_leave(g);\n      if (lj_state_gcprep_pending_acq(g) != 0)\n\tbreak;\n      if (!step)\n\tbreak;\n      if (finished_arena) {','      gc2_sweep_reclaim_leave(g);\n      /* Retain the exact arena/cursor and finish all writer cleanup before\n      ** ending this quantum. A suspended publisher cannot advance while a\n      ** nested full collector repeatedly consumes its cursor work. */\n      if (root_owner_blocked)\n\tgc2_quantum_defer(g);\n      if (lj_state_gcprep_pending_acq(g) != 0)\n\tbreak;\n      if (!step)\n\tbreak;\n      if (finished_arena) {')
change('lj_gc2.c','\tbreak;\n      }\n      n++;\n      continue;\n    }\n    if (lj_tg_flags_test_acq(tg, TGF_HUGETAB) &&','\tbreak;\n      }\n      n++;\n      if (root_owner_blocked)\n\tbreak;  /* Keep committed work counters, but do not retry this owner. */\n      continue;\n    }\n    if (lj_tg_flags_test_acq(tg, TGF_HUGETAB) &&')
change('lj_gc2.c','    n += lj_gc2_sweep_owner_progress(g, tg, limit - n, &finished);\n    if (finished)','    n += lj_gc2_sweep_owner_progress(g, tg, limit - n, &finished);\n    if (finished || gc2_deferred_epoch_acq(g) != defer0)')
patch=''
changes=[]
for rel,want in inputs.items():
 if sha(P/'candidate'/rel)!=want:
  changes.append(rel)
  patch+=''.join(difflib.unified_diff((S/rel).read_text().splitlines(keepends=True),(P/'candidate'/rel).read_text().splitlines(keepends=True),fromfile='a/'+rel,tofile='b/'+rel))
assert changes==['src/lj_gc.c','src/lj_gc.h','src/lj_gc2.c'],changes
(P/'candidate.patch').write_text(patch)
(P/'setup.py').write_text(Path('/tmp/lj-reclaim-defer-setup.py').read_text())
(P/'initial-source-validation.json').write_text(json.dumps({'changed':changes,'input_count':len(inputs),'patch_sha256':sha(P/'candidate.patch')},indent=2)+'\n')
print(P)
print(patch)
