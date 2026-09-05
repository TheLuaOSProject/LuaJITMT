from pathlib import Path
import subprocess,os,signal,time,json,sys,hashlib,resource
P=Path(__file__).resolve().parent;variant=sys.argv[1];T=P/variant;rows=[]
flags=['-DLJ_FUNC_TEST_HELPERS','-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLUA_USE_ASSERT']
env=os.environ.copy();env.pop('ASAN_OPTIONS',None)
if variant=='asan':env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
def run(name,cmd,timeout=90):
 start=time.monotonic();q=subprocess.Popen(cmd,env=env,cwd=T,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try:out,err=q.communicate(timeout=timeout);status='complete'
 except subprocess.TimeoutExpired:os.killpg(q.pid,signal.SIGKILL);out,err=q.communicate();status='timeout'
 row={'name':name,'command':cmd,'cwd':str(T),'ASAN_OPTIONS':env.get('ASAN_OPTIONS'),'exit':q.returncode,'status':status,'seconds':time.monotonic()-start,'stdout':out,'stderr':err}
 rows.append(row);(P/(variant+'-fixtures.json')).write_text(json.dumps(rows,indent=2)+'\n');print(name,q.returncode,round(row['seconds'],3),flush=True)
 if q.returncode:print(out[-3000:],err[-5000:],flush=True)
 return q.returncode
cases=[('t-gc2-sweep-table-coalescing',[[]]),('t-gc2-traverse',[[]]),('t-x64-tnew-empty-inline',[[]]),('t-jit-fnew-bump',[[]]),('t-arena-huge-tail',[[]]),('t-meta-cdata-capture',[[m] for m in ['basic','alias-source','alias-key','same-source-key','set-alias','retry-source','retry-key','retry-mt','retry-method','replace','growth','fail-growth','throw']]),('t-gc2-recovery',[[]]),('t-gc2-table-store-guard',[[]]),('t-gc2-finreg-cdata-preclaim-roots',[[]]),('t-gc2-finreg-udata-roots',[[]]),('t-m8-ffi-weak-newindex',[[]]),('t-m8-close-finalizers',[[]]),('t-jit-root-abort-retire',[[]])]
for name,args in cases:
 source=T/'tests'/(name+'.c')
 if not source.exists():
  print('SOURCE MISSING',source,flush=True);continue
 exe=P/(variant+'-'+name)
 cc=['clang','-O1','-fsanitize=address','-fno-omit-frame-pointer'] if variant=='asan' else ['gcc','-O2']
 cmd=['taskset','-c','0-15']+cc+['-std=gnu11','-g','-Wall','-Wextra','-Werror','-mcx16']+flags+['-I'+str(T/'src'),str(source),str(T/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)]
 if name=='t-gc2-sweep-table-coalescing':cmd+=['-DLJ_TEST_WRAP_CALLOC','-Wl,--wrap=calloc']
 if name=='t-arena-huge-tail':cmd+=['-Wl,--wrap='+s for s in ['mmap','mmap64','munmap','calloc','free']]
 if name=='t-meta-cdata-capture':cmd+=['-D_GNU_SOURCE']+['-Wl,--wrap='+s for s in ['lj_gc2_tv_lease_acquire','lj_gc2_lease_release','lj_tab_wait_l','lj_gc2_smr_read_enter','lj_vm_call','lj_vm_pcall','lj_vm_cpcall','lj_vm_resume']]
 if run('compile-'+name,cmd):continue
 for a in args:run(name+('-'+a[0] if a else ''),['taskset','-c','0-15',str(exe)]+a,30 if name=='t-meta-cdata-capture' else 90)
print('Fixture run complete',variant,flush=True)
