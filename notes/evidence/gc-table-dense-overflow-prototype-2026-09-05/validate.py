from pathlib import Path
import os,subprocess,json,time,signal,sys,hashlib
p=Path(__file__).parent
variant=sys.argv[1] if len(sys.argv)>1 else 'strict'
t=p/variant; rows=[]
flags=['-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_FUNC_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLUA_USE_ASSERT']
def run(name,cmd,timeout=90,cwd=None,env=None):
 start=time.monotonic()
 q=subprocess.Popen(cmd,cwd=cwd,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try:out,err=q.communicate(timeout=timeout);status='complete'
 except subprocess.TimeoutExpired:os.killpg(q.pid,signal.SIGKILL);out,err=q.communicate();status='timeout'
 r={'name':name,'command':cmd,'cwd':str(cwd) if cwd else None,'exit':q.returncode,'status':status,'seconds':time.monotonic()-start,'stdout':out,'stderr':err}
 rows.append(r);(p/(variant+'-results.json')).write_text(json.dumps(rows,indent=2)+'\n')
 print(name,q.returncode,flush=True)
 if q.returncode:print(out,err,flush=True)
 return r
for name,source,args in [
 ('overflow',p/'t-dense-overflow.c',[['all'],['existing']]),
 ('tnew',p/'t-dense-tnew.c',[['1536'],['1537'],['1536','1'],['1537','1'],['existing']]),
 ('fnew',p/'t-dense-fnew-settled.c',[['1536'],['1537'],['existing']]),
 ('traverse',p/'traverse-adapter.c',[[]]),
 ('recovery',t/'tests/t-gc2-recovery.c',[[]]),
 ('guard',t/'tests/t-gc2-table-store-guard.c',[[]]),
]:
 exe=p/(variant+'-'+name)
 compiler=['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16']
 env=None
 if variant=='asan':
  compiler=['clang','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-mcx16','-fsanitize=address','-fno-omit-frame-pointer']
  env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
 cmd=compiler+flags+['-I'+str(t/'src'),'-I'+str(t/'tests'),'-I'+str(p),str(source),str(t/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)]
 if name=='overflow':cmd+=['-DDENSE_WRAP_CALLOC','-Wl,--wrap=calloc']
 r=run('compile-'+name,cmd)
 if r['exit']:continue
 for a in args:
  run(name+'-'+('-'.join(a) or 'all'),['taskset','-c','0-15',str(exe)]+a,env=env)
print('finished',variant,flush=True)

