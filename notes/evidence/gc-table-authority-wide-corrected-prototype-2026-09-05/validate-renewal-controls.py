from pathlib import Path
import shutil,subprocess,json,time,os,signal,hashlib
r=Path(__file__).parent
neg=r/'negative-era'
if not neg.exists(): shutil.copytree(r/'strict',neg)
p=neg/'src/lj_gc2.c';s=p.read_text()
a='''    if (old.hi != captured.hi ||
        gc2_tabstamp_dirty(old.lo) != gc2_tabstamp_dirty(captured.lo))'''
b='''    if (gc2_tabstamp_dirty(old.lo) != gc2_tabstamp_dirty(captured.lo))'''
if a in s:
 s=s.replace(a,b).replace('    next.hi = captured.hi;','    next.hi = old.hi;',1);p.write_text(s)
(r/'t-wide-stamp-controls.c').write_text((r/'t-wide-stamp.c').read_text().replace('int main(int argc, char **argv)', 'int wide_main(int argc, char **argv)') + '''
int main(int argc, char **argv)
{
 int huge; assert(argc == 3); huge = atoi(argv[2]); assert(huge == 0 || huge == 1); alarm(40);
 if (!strcmp(argv[1], "pause")) test_old_scanner_renewal(huge);
 else { assert(!strcmp(argv[1], "collect")); test_continued_collection(huge); }
 puts("wide-stamp focused control OK"); return 0;
}
''')
flags=['-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_FUNC_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLUA_USE_ASSERT'];rows=[]
def run(name,cmd,timeout=50):
 start=time.monotonic();p=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try:o,e=p.communicate(timeout=timeout);status='complete'
 except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);o,e=p.communicate();status='timeout'
 row={'name':name,'command':cmd,'rc':p.returncode,'status':status,'elapsed':time.monotonic()-start,'stdout':o,'stderr':e};rows.append(row);(r/'renewal-control-results.json').write_text(json.dumps(rows,indent=2)+'\n');print(name,p.returncode,o,e,flush=True);return row
row=run('build-negative-era',['taskset','-c','0-15','make','-C',str(neg/'src'),'-j4','BUILDMODE=static','XCFLAGS='+' '.join(flags)])
assert row['rc']==0
for v in ('strict','negative-era','baseline-strict'):
 t=r/v;ex=r/(v+'-renewal-controls')
 cmd=['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16']+flags+['-I'+str(t/'src'),'-I'+str(t/'tests'),str(r/'t-wide-stamp-controls.c'),str(t/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(ex)]
 if run('compile-'+v,cmd)['rc']:continue
 for huge in ('0','1'):
  modes=('pause','collect') if v=='strict' else ('pause',) if v=='negative-era' else ('collect',)
  for mode in modes:run(v+'-'+mode+'-'+huge,['taskset','-c','0-15',str(ex),mode,huge])
