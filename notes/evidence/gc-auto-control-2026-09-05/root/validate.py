from pathlib import Path
import hashlib,json,os,subprocess,sys,time,resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent;repo=Path('/workspaces/lj-lockless');owner=p;kind=sys.argv[1];tree=owner/kind;asan=kind=='asan';strict=kind!='candidate';cc='clang' if asan else 'cc'
flags=['-DLUA_USE_APICHECK','-DLUA_USE_ASSERT','-DLJ_FUNC_TEST_HELPERS','-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_XSAVE_TEST_HELPERS'] if strict else []
san=['-fsanitize=address','-fno-omit-frame-pointer'] if asan else []
env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';'+str(repo/'tests/lib/?.lua')+';;'
if asan:env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
else:env.pop('ASAN_OPTIONS',None)
rows=[];binaries={}
def sha(f):return hashlib.sha256(f.read_bytes()).hexdigest()
def run(name,cmd,cwd=tree,bound=30,test=True,inputs=[]):
 start=time.monotonic()
 try:
  r=subprocess.run(cmd,cwd=cwd,env=env,text=True,capture_output=True,timeout=bound);res=dict(exit=r.returncode,stdout=r.stdout,stderr=r.stderr)
 except subprocess.TimeoutExpired as e:res=dict(exit=None,timeout=True,stdout=(e.stdout or b'').decode(errors='replace'),stderr=(e.stderr or b'').decode(errors='replace'))
 row=dict(name=name,command=cmd,cwd=str(cwd),test=test,seconds=time.monotonic()-start,environment={k:env[k] for k in ['LUA_PATH','ASAN_OPTIONS','LJ_M7_FFI_CLIB_EXTERN_SO','LJ_M7_FFI_CALLXS_FLUSH_SO'] if k in env},inputs={str(f):sha(f) for f in inputs},**res)
 rows.append(row);(p/(kind+'-results.json')).write_text(json.dumps(rows,indent=2)+'\n');print(kind,name,'exit',res['exit'],flush=True);assert res['exit']==0,res
exe=tree/'src/luajit'
for mode in ['-joff','-jon']:run('stock'+mode,['taskset','-c','0-15',str(exe),mode,'test.lua','--quiet'],cwd=repo/'tests/stock/test',inputs=[exe])
for mode in ['function','table','missing','nonfunction','replace','replace_missing','resize','methodlife','table_entry','newindex','newindex_table']:
 f=repo/'tests/t-jit-special-udata-guards.lua';run('clib-method '+mode,['taskset','-c','0-15',str(exe),'-jon',str(f),'clib',mode],inputs=[exe,f])

if asan:
 host=run('asan-host-check',['nm',str(tree/'src/host/minilua'),str(tree/'src/host/buildvm')],test=False,inputs=[tree/'src/host/minilua',tree/'src/host/buildvm'])
 target=run('asan-target-check',['nm',str(tree/'src/lj_crecord.o'),str(tree/'src/lj_ir.o')],test=False,inputs=[tree/'src/lj_crecord.o',tree/'src/lj_ir.o'])
 assert '__asan_' not in rows[-2]['stdout'] and '__asan_' in rows[-1]['stdout']
libs=[]
for value in [11,29]:
 f=repo/'tests/t-ffi-clib-receiver-lib.c';lib=p/(kind+'-receiver-'+str(value)+'.so')
 run('compile-receiver-'+str(value),[cc,'-shared','-fPIC','-O2','-Wall','-Wextra','-Werror','-DNAMESPACE_VALUE='+str(value),str(f),'-o',str(lib)],test=False,inputs=[f]);libs.append(lib)
for mode in ['index-other','index-type','newindex-other','newindex-type','index-life','newindex-life','index-side-other','index-side-type','newindex-side-other','newindex-side-type']:
 for status in ['-joff','-jon']:
  f=repo/'tests/t-ffi-clib-receiver.lua';run('receiver '+mode+' '+status,['taskset','-c','0-15',str(exe),status,str(f),mode]+[str(x) for x in libs],inputs=[exe,f]+libs)
for obj in ['clib','file','buffer','plain']:
 for mode in ['function','table','missing','nonfunction','resize','replace','replace_missing','table_entry','methodlife']:
  f=repo/'tests/t-jit-udata-pure.lua';run('direct '+obj+' '+mode,['taskset','-c','0-15',str(exe),'-jon',str(f),obj,mode],inputs=[exe,f])
for mode in ['allocate','luastore','newref','clear','foreign','indirect','fpmath','directstore']:
 f=repo/'tests/t-jit-udata-pure-exclusions.lua';run('udata-effects '+mode,['taskset','-c','0-15',str(exe),'-jon',str(f),mode],inputs=[exe,f])
for script,modes in [('t-jit-cdata-pure.lua',['index','newindex','missing','nonfunction','resize','methodlife','replace']),('t-jit-cdata-pure-exclusions.lua',['allocate','luastore','newref','clear','foreign','indirect','fpmath']),('t-jit-cdata-pure-side.lua',[None]),('t-jit-cdata-pure-profile.lua',[None])]:
 for mode in modes:
  f=repo/'tests'/script;run(script+' '+str(mode),['taskset','-c','0-15',str(exe),'-jon',str(f)]+([mode] if mode else []),inputs=[exe,f])

extern=p/(kind+'-extern.so');f=repo/'tests/t-ffi-clib-extern-snapshot-lib.c';run('compile-extern-library',[cc,'-shared','-fPIC','-O2',str(f),'-o',str(extern)],test=False,inputs=[f]);env['LJ_M7_FFI_CLIB_EXTERN_SO']=str(extern)
for name in ['t-ffi-recorder-libmeta-busy.c','t-ffi-clib-extern-snapshot.c','t-ffi-clib-cache-retire.c','t-ffi-callxs-callback-stack.c']:
 f=repo/'tests'/name;out=p/(kind+'-'+name[:-2]);cmd=[cc,'-std=gnu11','-O1' if asan else '-O2','-g','-Wall','-Wextra','-Werror','-mcx16']+flags+san+['-I'+str(tree/'src'),'-I'+str(repo/'tests'),str(f),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(out)]
 run('compile-'+name,cmd,test=False,inputs=[f,tree/'src/libluajit.a']);run(name,['taskset','-c','0-15',str(out)],inputs=[out,extern]);binaries[out.name]=sha(out)
remote=p/(kind+'-remote.so');f=repo/'tests/t-ffi-callxs-remote-flush-lib.c';run('compile-remote-library',[cc,'-shared','-fPIC','-O2',str(f),'-o',str(remote)],test=False,inputs=[f]);env['LJ_M7_FFI_CALLXS_FLUSH_SO']=str(remote)
f=repo/'tests/t-ffi-callxs-remote-flush.lua';run('generated-remote-flush',['taskset','-c','0-15',str(exe),'-jon',str(f)],inputs=[f,remote,exe])
(p/(kind+'-binaries.json')).write_text(json.dumps({**binaries,'luajit':sha(exe),'libluajit.a':sha(tree/'src/libluajit.a'),'extern.so':sha(extern),'remote.so':sha(remote)},indent=2)+'\n')
