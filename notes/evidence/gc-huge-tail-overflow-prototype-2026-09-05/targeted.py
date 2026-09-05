from pathlib import Path
import subprocess,time,json,os,signal,sys
p=Path(__file__).parent;variant=sys.argv[1] if len(sys.argv)>1 else 'tail';rows=[]
flags=['-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_FUNC_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLUA_USE_ASSERT']
def run(name,cmd,env=None):
 start=time.monotonic();q=subprocess.Popen(cmd,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try:out,err=q.communicate(timeout=60);status='complete'
 except subprocess.TimeoutExpired:os.killpg(q.pid,signal.SIGKILL);out,err=q.communicate();status='timeout'
 rows.append({'name':name,'command':cmd,'exit':q.returncode,'status':status,'seconds':time.monotonic()-start,'stdout':out,'stderr':err});(p/(variant+'-targeted.json')).write_text(json.dumps(rows,indent=2)+'\n');print(name,q.returncode,out,err,flush=True);return q.returncode
compiler=['gcc','-O2'];env=None
if 'asan' in variant:compiler=['clang','-O1','-fsanitize=address','-fno-omit-frame-pointer'];env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
exe=p/(variant+'-targeted')
cmd=['taskset','-c','0-15']+compiler+['-std=gnu11','-g','-Wall','-Wextra','-Werror','-mcx16']+flags+['-I'+str(p/variant/'src'),str(p/'t-huge-tail.c'),str(p/variant/'src/libluajit.a'),'-lm','-ldl','-pthread','-Wl,--wrap=mmap','-Wl,--wrap=mmap64','-Wl,--wrap=munmap','-Wl,--wrap=calloc','-Wl,--wrap=free','-o',str(exe)]
if not run('compile',cmd):
 for mode in ['geometry','payload','bounds','resize','failures']:run(mode,['taskset','-c','0-15',str(exe),mode],env)
