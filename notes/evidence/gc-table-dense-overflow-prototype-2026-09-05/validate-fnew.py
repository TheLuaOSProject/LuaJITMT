from pathlib import Path
import subprocess,os,signal,time,json
p=Path(__file__).parent;rows=[]
flags=['-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_FUNC_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLUA_USE_ASSERT']
def run(name,cmd,env=None):
 start=time.monotonic();q=subprocess.Popen(cmd,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try:out,err=q.communicate(timeout=90);status='complete'
 except subprocess.TimeoutExpired:os.killpg(q.pid,signal.SIGKILL);out,err=q.communicate();status='timeout'
 rows.append({'name':name,'command':cmd,'exit':q.returncode,'status':status,'seconds':time.monotonic()-start,'stdout':out,'stderr':err})
 (p/'fnew-consistent-results.json').write_text(json.dumps(rows,indent=2)+'\n');print(name,q.returncode,out[-500:],err[-1000:],flush=True)
 return q.returncode
for variant in ['strict','asan']:
 compiler=['gcc','-O2'];env=None
 if variant=='asan':
  compiler=['clang','-O1','-fsanitize=address','-fno-omit-frame-pointer'];env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
 for name in ['t-dense-fnew-consistent','fnew-invalid-allocator-control']:
  exe=p/(variant+'-'+name)
  cmd=['taskset','-c','0-15']+compiler+['-std=gnu11','-g','-Wall','-Wextra','-Werror','-mcx16']+flags+['-I'+str(p/variant/'src'),'-I'+str(p/variant/'tests'),str(p/(name+'.c')),str(p/variant/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)]
  if not run('compile-'+variant+'-'+name,cmd):run(variant+'-'+name,['taskset','-c','0-15',str(exe)],env)
