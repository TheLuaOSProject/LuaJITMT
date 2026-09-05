from pathlib import Path
import subprocess, os, signal, json, time, hashlib
P=Path(__file__).resolve().parent;T=P/'tree';rows=[]
flags=['-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_FUNC_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLUA_USE_ASSERT']
def run(name,cmd):
 start=time.monotonic();q=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try:out,err=q.communicate(timeout=120);status='complete'
 except subprocess.TimeoutExpired:os.killpg(q.pid,signal.SIGKILL);out,err=q.communicate();status='timeout'
 rows.append({'name':name,'command':cmd,'exit':q.returncode,'status':status,'seconds':time.monotonic()-start,'stdout':out,'stderr':err});(P/'strict-results.json').write_text(json.dumps(rows,indent=2)+'\n');print(name,q.returncode,flush=True)
 if q.returncode:print(out[-2000:],err[-3000:],flush=True)
 return q.returncode
for name in ['t-gc2-sweep-table-coalescing','t-gc2-traverse','t-x64-tnew-empty-inline','t-jit-fnew-bump','t-arena-huge-tail']:
 source=T/'tests'/(name+'.c');exe=P/name
 cmd=['taskset','-c','0-15','gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16']+flags+['-I'+str(T/'src'),str(source),str(T/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)]
 if name=='t-gc2-sweep-table-coalescing':cmd+=['-DLJ_TEST_WRAP_CALLOC','-Wl,--wrap=calloc']
 if name=='t-arena-huge-tail':cmd+=['-Wl,--wrap=mmap','-Wl,--wrap=mmap64','-Wl,--wrap=munmap','-Wl,--wrap=calloc','-Wl,--wrap=free']
 if not run('compile-'+name,cmd):run(name,['taskset','-c','0-15',str(exe)])
print('Strict integration complete',flush=True)
