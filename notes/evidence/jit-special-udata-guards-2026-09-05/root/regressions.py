from pathlib import Path
import hashlib,json,os,resource,subprocess,sys,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent;kind=sys.argv[1];tree=p/kind
strict=kind in ['strict','asan'];asan=kind=='asan';cc='clang' if asan else 'cc'
flags=['-DLUA_USE_ASSERT','-DLJ_FUNC_TEST_HELPERS','-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_XSAVE_TEST_HELPERS'] if strict else []
san=['-fsanitize=address','-fno-omit-frame-pointer'] if asan else []
env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';'+str(tree/'tests/lib/?.lua')+';;'
if asan:env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
else:env.pop('ASAN_OPTIONS',None)
rows=[];binaries={}
def run(name,cmd,bound=30,cwd=tree,test=True):
 start=time.monotonic()
 try:
  r=subprocess.run(cmd,cwd=cwd,env=env,capture_output=True,text=True,timeout=bound)
  result=dict(exit=r.returncode,stdout=r.stdout,stderr=r.stderr)
 except subprocess.TimeoutExpired as e:
  result=dict(exit=None,timeout=True,stdout=(e.stdout or b'').decode(errors='replace'),stderr=(e.stderr or b'').decode(errors='replace'))
 rows.append(dict(name=name,test=test,command=cmd,cwd=str(cwd),seconds=time.monotonic()-start,environment={k:env[k] for k in ['LUA_PATH','ASAN_OPTIONS','LJ_M7_FFI_CALLXS_FLUSH_SO'] if k in env},**result))
 (p/(kind+'-regression-results.json')).write_text(json.dumps(rows,indent=2)+'\n')
 print(kind,name,'exit',result['exit'],flush=True)
 assert result['exit']==0,result
for mode in ['-joff','-jon']:
 run('stock'+mode,['taskset','-c','0-15',str(tree/'src/luajit'),mode,'test.lua','--quiet'],cwd=tree/'tests/stock/test')
 run('cdata-base-guards'+mode,['taskset','-c','0-15',str(tree/'src/luajit'),mode,str(tree/'tests/t-jit-cdata-basemt-guards.lua')])
for f in ['t-jit-cdata-pure.lua','t-jit-cdata-pure-side.lua','t-jit-cdata-pure-exclusions.lua','t-jit-cdata-pure-profile.lua']:
 run(f,['taskset','-c','0-15',str(tree/'src/luajit'),'-jon',str(tree/'tests'/f)])
fixtures=[('t-ffi-callxs-callback-stack.c',[[]],[]),('t-jit-first-attach.c',[[],['noloop']],[])]
if strict:fixtures.append(('t-jit-xsave.c',[[]],[]))
for f,argslist,extra in fixtures:
 exe=p/(kind+'-'+f[:-2]);cmd=[cc,'-std=gnu11','-O1' if asan else '-O2','-g','-Wall','-Wextra','-Werror','-mcx16']+flags+san+['-I'+str(tree/'src'),'-I'+str(tree/'tests'),str(tree/'tests'/f),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread']+extra+['-o',str(exe)]
 run('compile-'+f,cmd,test=False);binaries[exe.name]=hashlib.sha256(exe.read_bytes()).hexdigest()
 for args in argslist:run(f+' '+' '.join(args),['taskset','-c','0-15',str(exe)]+args)
(p/(kind+'-regression-binaries.json')).write_text(json.dumps(binaries,indent=2)+'\n')
lib=p/(kind+'-remote-flush.so')
run('compile-remote-library',[cc,'-O2','-shared','-fPIC',str(tree/'tests/t-ffi-callxs-remote-flush-lib.c'),'-o',str(lib)],test=False)
env['LJ_M7_FFI_CALLXS_FLUSH_SO']=str(lib)
run('generated-remote-flush',['taskset','-c','0-15',str(tree/'src/luajit'),'-jon',str(tree/'tests/t-ffi-callxs-remote-flush.lua')])
