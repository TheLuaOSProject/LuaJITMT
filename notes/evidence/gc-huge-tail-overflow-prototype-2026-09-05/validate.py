from pathlib import Path
import os,subprocess,json,time,signal,sys
p=Path(__file__).parent;variant=sys.argv[1] if len(sys.argv)>1 else 'tail';t=p/variant;rows=[]
only=sys.argv[2] if len(sys.argv)>2 else ''
result_path=p/(variant+'-validation'+('-'+only if only else '')+'.json')
flags=['-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_FUNC_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLUA_USE_ASSERT']
def run(name,cmd,env=None):
 start=time.monotonic();q=subprocess.Popen(cmd,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try:out,err=q.communicate(timeout=100);status='complete'
 except subprocess.TimeoutExpired:os.killpg(q.pid,signal.SIGKILL);out,err=q.communicate();status='timeout'
 rows.append({'name':name,'command':cmd,'exit':q.returncode,'status':status,'seconds':time.monotonic()-start,'stdout':out,'stderr':err});result_path.write_text(json.dumps(rows,indent=2)+'\n');print(name,q.returncode,flush=True)
 if q.returncode:print(out[-2000:],err[-3000:],flush=True)
 return q.returncode
cases=[('overflow',p/'t-dense-overflow.c',[['all'],['existing']]),('tnew',p/'t-dense-tnew.c',[['1536'],['1537'],['1536','1'],['1537','1'],['existing']]),('fnew',p/'t-dense-fnew.c',[['1536'],['1537']]),('traverse',p/'traverse-adapter.c',[[]]),('recovery',t/'tests/t-gc2-recovery.c',[[]]),('guard',t/'tests/t-gc2-table-store-guard.c',[[]]),('arena-huge',t/'tests/t-arena-huge.c',[[]]),('arena-hugetab',t/'tests/t-arena-hugetab.c',[[]]),('arena-realloc',t/'tests/t-arena-realloc.c',[[]]),('arena-gcsweep',t/'tests/t-arena-gcsweep.c',[[]]),('terminal-orphan',t/'tests/t-tg-terminal-orphan.c',[[]])]
for name,source,args in cases:
 if only and name!=only:continue
 compiler=['gcc','-O2'];env=None
 if 'asan' in variant:compiler=['clang','-O1','-fsanitize=address','-fno-omit-frame-pointer'];env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
 exe=p/(variant+'-'+name)
 cmd=['taskset','-c','0-15']+compiler+['-std=gnu11','-g','-Wall','-Wextra','-Werror','-mcx16']+flags+['-I'+str(t/'src'),'-I'+str(t/'tests'),'-I'+str(p),str(source),str(t/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)]
 if name=='overflow':cmd+=['-DDENSE_WRAP_CALLOC','-Wl,--wrap=calloc']
 if name=='terminal-orphan':cmd+=['-Wl,--wrap=lj_arena_hugetab_transfer','-Wl,--wrap=munmap']
 if run('compile-'+name,cmd):continue
 for a in args:run(name+'-'+('-'.join(a) or 'all'),['taskset','-c','0-15',str(exe)]+a,env)
print('finished',variant,flush=True)
