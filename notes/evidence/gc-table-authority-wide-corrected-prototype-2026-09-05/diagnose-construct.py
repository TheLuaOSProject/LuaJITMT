from pathlib import Path
import subprocess,json,time,os,signal
r=Path(__file__).parent
s=(r/'fnew-prerequisites.c').read_text()
diag='''static void dump_construct(TGState *tg, const char *at)
{
 GCArena *a; unsigned list,i; GCArena *heads[3] = {tg->alloc.owned[0], tg->alloc.needsweep[0], tg->alloc.quarantine[0]};
 fprintf(stderr, "AT %s phase %u cycle %u\\n", at, gc2_phase_acq(tg->gl), gc2_cycle_acq(tg->gl));
 for(list=0;list<3;list++) for(a=heads[list];a;a=lj_arena_next_acq(a)) for(i=LJ_AFIRST_CELL;i<LJ_ARENA_CELLS;i++) {
   uint32_t life=lj_arena_lifetime_state_acq(a,i), root=lj_arena_root_state_acq(a,i);
   if(life==LJ_ARENA_LIFETIME_CONSTRUCT || root==LJ_ARENA_ROOT_LINKING)
     fprintf(stderr,"  list%u a%p cell%u life%u root%u block%d ready%d gct%u\\n",list,(void*)a,i,life,root,lj_arena_bm_get(a->block,i),lj_arena_ready_get(a,i),((GCobj *)lj_arena_cellptr(a,i))->gch.gct);
 }
}
'''
s=s.replace('int main(void)\n',diag+'\nint main(void)\n')
start=s.index('int main(void)\n')
prefix=s[:start];main=s[start:]
lines=[]
for l in main.splitlines():
 lines.append(l)
 if l.startswith('  test_'):lines.append('  dump_construct(tg, "'+l.strip().split('(')[0]+'");')
s=prefix+'\n'.join(lines)+'\n';(r/'fnew-construct-diag.c').write_text(s)
flags=['-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_FUNC_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLUA_USE_ASSERT']
rows=[]
for v in ('baseline-strict','strict'):
 t=r/v;e=r/(v+'-fnew-construct-diag')
 cmd=['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16']+flags+['-I'+str(t/'src'),'-I'+str(t/'tests'),str(r/'fnew-construct-diag.c'),str(t/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(e)]
 q=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True);rows.append({'variant':v,'kind':'compile','command':cmd,'rc':q.returncode,'stdout':q.stdout,'stderr':q.stderr})
 if q.returncode:continue
 cmd=['taskset','-c','0-15',str(e)];start=time.monotonic();p=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try:o,err=p.communicate(timeout=5);status='complete'
 except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);o,err=p.communicate();status='timeout'
 (r/(v+'-construct.stdout')).write_text(o);(r/(v+'-construct.stderr')).write_text(err)
 rows.append({'variant':v,'kind':'run','command':cmd,'rc':p.returncode,'status':status,'elapsed':time.monotonic()-start,'stdout':o,'stderr':err})
(r/'fnew-construct-results.json').write_text(json.dumps(rows,indent=2)+'\n')
for x in rows:
 if x['kind']=='run':print(x['variant'],x['rc'],x['stderr'][:12000])
