from pathlib import Path
import subprocess,os,signal,time,json,shutil,difflib
p=Path(__file__).parent;rows=[]
flags='-DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS -DLJ_FUNC_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLUA_USE_ASSERT'
source=p/'t-dense-overflow.c';s=source.read_text()
s=s.replace('  alarm(60);','''  alarm(60);
  if (!strcmp(mode, "inline")) { test_old_scanner(0, 0, 1); return 0; }
  if (!strcmp(mode, "wide")) { test_old_scanner(0, 1, 1); return 0; }
  if (!strcmp(mode, "mode")) { test_mode_pause(0, LJ_GC2_TABLE_COALESCE_TEST_POST_MODE); return 0; }''',1)
(p/'t-dense-negative.c').write_text(s)
def run(name,cmd,cwd=None):
 start=time.monotonic();q=subprocess.Popen(cmd,cwd=cwd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try:out,err=q.communicate(timeout=90);status='complete'
 except subprocess.TimeoutExpired:os.killpg(q.pid,signal.SIGKILL);out,err=q.communicate();status='timeout'
 rows.append({'name':name,'command':cmd,'cwd':str(cwd) if cwd else None,'exit':q.returncode,'status':status,'seconds':time.monotonic()-start,'stdout':out,'stderr':err})
 (p/'negative-results.json').write_text(json.dumps(rows,indent=2)+'\n');print(name,q.returncode,out[-500:],err[-1000:],flush=True)
 return q.returncode
orig=(p/'strict/src/lj_gc2.c').read_text()
for mode in ['inline','wide','mode']:
 t=p/('negative-'+mode);(t/'src').mkdir(parents=True,exist_ok=True)
 for f in (p/'strict/src').iterdir():
  if f.is_file():shutil.copy2(f,t/'src'/f.name)
 # Include nested DynASM host files without copying tests or rebuilding generators.
 for f in (p/'strict/src').iterdir():
  if f.is_dir():shutil.copytree(f,t/'src'/f.name,dirs_exist_ok=True)
 shutil.copytree(p/'strict/dynasm',t/'dynasm',dirs_exist_ok=True)
 s=orig
 if mode=='inline':
  old='''  if (!s || !captured.stamped)
    return 0;
  if (captured.wide) {'''
  new='''  if (!s || !captured.stamped)
    return 0;
  /* NEGATIVE: completion adopts the current domain after its payload scan. */
  if (!captured.wide && gc2_tabstamp_dirty(la_load64_acq(&s->state)) == UINT32_MAX) {
    captured.wide = 1;
    captured.proof = lj_arena_gc2_wide_snapshot(lj_arena_gc2_wide_acq(t));
    dirty = gc2_tabstamp_dirty(captured.proof.lo);
  }
  if (captured.wide) {'''
  assert old in s;s=s.replace(old,new,1)
 elif mode=='wide':
  old='if (old.hi != captured.proof.hi || gc2_tabstamp_dirty(old.lo) != dirty)'
  assert old in s;s=s.replace(old,'if (gc2_tabstamp_dirty(old.lo) != dirty)',1)
 else:
  old='''      prior = lj_arena_gc2_wide_snapshot(w);'''
  new='''      /* NEGATIVE: expose W mode before invalidating old W coverage. */
      while (gc2_tabstamp_dirty(old) != UINT32_MAX) {
        if (la_cas64(&s->state, &old, (uint64_t)UINT32_MAX, LA_ACQ_REL, LA_ACQ)) break;
      }
      gc2_table_coalesce_test_at(g, t, LJ_GC2_TABLE_COALESCE_TEST_POST_MODE);
      prior = lj_arena_gc2_wide_snapshot(w);'''
  assert old in s;s=s.replace(old,new,1)
 (t/'src/lj_gc2.c').write_text(s)
 (p/('negative-'+mode+'.patch')).write_text(''.join(difflib.unified_diff(orig.splitlines(True),s.splitlines(True),fromfile='a/src/lj_gc2.c',tofile='b/src/lj_gc2.c')))
 if run('build-'+mode,['taskset','-c','0-15','make','-C',str(t/'src'),'-j4','BUILDMODE=static','XCFLAGS='+flags,'CCDEBUG=-g','TARGET_STRIP=:']):continue
 exe=p/('negative-'+mode+'-fixture')
 cmd=['taskset','-c','0-15','gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16']+flags.split()+['-I'+str(t/'src'),'-I'+str(p/'strict/tests'),'-I'+str(p),str(p/'t-dense-negative.c'),str(t/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)]
 if run('compile-'+mode,cmd):continue
 run('test-'+mode,['taskset','-c','0-15',str(exe),mode])
