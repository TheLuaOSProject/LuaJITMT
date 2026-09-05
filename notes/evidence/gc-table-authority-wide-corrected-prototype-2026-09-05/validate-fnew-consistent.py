from pathlib import Path
import subprocess,json,time,os,signal,difflib
r=Path(__file__).parent
old=(r/'fnew-prerequisites.c').read_text()
s=old.replace('#include "lj_target.h"','#include "lj_target.h"\n#include "lj_tab.h"')
s=s.replace('  uint32_t old_allocf_arena = la_load32_acq(&g->allocf_arena);\n\n  /* RD!=0', '  uint32_t old_allocf_arena = la_load32_acq(&g->allocf_arena);\n  uint32_t helper0;\n\n  /* RD!=0',1)
a='''  la_store32_rel(&g->allocf_arena, 0);
  ljt_lua_loadstring(L, "return {}\\n");
  ljt_lua_pcall(L, 0, 1, "x64 TNEW eligibility fallback target");
  la_store32_rel(&g->allocf_arena, old_allocf_arena);'''
b='''  /* Keep allocator identity truthful: an arena allocation must finish its
  ** CONSTRUCT|LINKING lanes. A real MT-entry contender rejects the same VM
  ** >7 branch; the helper counter witnesses that branch directly. */
  ljt_lua_loadstring(L, "return {}\\n");
  helper0 = lj_tab_test_new0_calls();
  assert(mt_entering_add_rlx(g, 1) == 0);
  ljt_lua_pcall(L, 0, 1, "x64 TNEW eligibility fallback target");
  assert(mt_entering_sub_acqrel(g, 1) == 1);
  mt_entering_futex_wake(g, 0x7fffffff);
  assert(lj_tab_test_new0_calls() == helper0 + 1u);
  assert(la_load32_acq(&g->allocf_arena) == old_allocf_arena);'''
assert s.count(a)==1;s=s.replace(a,b)
a='''  la_store32_rel(&g->allocf_arena, 0);
  assert_gc1num_bump_blocked(L, slots, child, &parent->l, slotno, 33);
  la_store32_rel(&g->allocf_arena, old_allocf_arena);'''
b='''  /* Recheck a second MT-entry fallback with fresh closure/cell bodies.
  ** This control does not directly exercise the allocf_arena identity gate:
  ** falsifying it while retaining lj_arena_allocf skips construction commit. */
  assert(mt_entering_add_rlx(g, 1) == 0);
  assert_gc1num_bump_blocked(L, slots, child, &parent->l, slotno, 33);
  assert(mt_entering_sub_acqrel(g, 1) == 1);
  mt_entering_futex_wake(g, 0x7fffffff);
  assert(la_load32_acq(&g->allocf_arena) == old_allocf_arena);'''
assert s.count(a)==1;s=s.replace(a,b)
helper='''static void assert_no_unfinished_owned_constructors(TGState *tg)
{
  GCArena *a;
  uint32_t cell;
  /* Single-threaded fixture boundary: every allocation call has returned. */
  for (a = tg->alloc.owned[LJ_ARENAK_TRAVERSABLE]; a;
       a = lj_arena_next_acq(a))
    for (cell = LJ_AFIRST_CELL; cell < LJ_ARENA_CELLS; cell++) {
      assert(lj_arena_lifetime_state_acq(a, cell) !=
             LJ_ARENA_LIFETIME_CONSTRUCT);
      assert(lj_arena_root_state_acq(a, cell) != LJ_ARENA_ROOT_LINKING);
    }
}

'''
for name,source in [('fnew-consistent-setup.c',s),('fnew-invalid-allocator-control.c',old)]:
 source=source.replace('int main(void)\n',helper+'int main(void)\n')
 source=source.replace('  test_vm_tnew_branch_targets(L, g);','  test_vm_tnew_branch_targets(L, g);\n  assert_no_unfinished_owned_constructors(tg);')
 source=source.replace('  test_bump_allocator_gate_direct(L, g, tg);','  test_bump_allocator_gate_direct(L, g, tg);\n  assert_no_unfinished_owned_constructors(tg);')
 (r/name).write_text(source)
 if name=='fnew-consistent-setup.c':
  baseline=(r/'strict/tests/t-jit-fnew-bump.c').read_text()
  (r/'fnew-consistent-setup.patch').write_text(''.join(difflib.unified_diff(baseline.splitlines(True),source.splitlines(True),fromfile='d680/tests/t-jit-fnew-bump.c',tofile=name)))
flags=['-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_FUNC_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLUA_USE_ASSERT']
rows=[]
for v in ('baseline-strict','strict'):
 for source in ('fnew-consistent-setup','fnew-invalid-allocator-control'):
  t=r/v;e=r/(v+'-'+source)
  cmd=['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16']+flags+['-I'+str(t/'src'),'-I'+str(t/'tests'),str(r/(source+'.c')),str(t/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(e)]
  q=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True);rows.append({'variant':v,'fixture':source,'kind':'compile','command':cmd,'rc':q.returncode,'stdout':q.stdout,'stderr':q.stderr})
  if q.returncode:continue
  cmd=['taskset','-c','0-15',str(e)];start=time.monotonic();p=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
  try:o,err=p.communicate(timeout=45);status='complete'
  except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);o,err=p.communicate();status='timeout'
  row={'variant':v,'fixture':source,'kind':'run','command':cmd,'rc':p.returncode,'status':status,'elapsed':time.monotonic()-start,'stdout':o,'stderr':err};rows.append(row);print(row,flush=True)
  (r/'fnew-consistent-results.json').write_text(json.dumps(rows,indent=2)+'\n')
