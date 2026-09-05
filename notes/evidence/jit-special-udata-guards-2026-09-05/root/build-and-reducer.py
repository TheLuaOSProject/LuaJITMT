from pathlib import Path
import hashlib,json,os,resource,subprocess,sys,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent
kind=sys.argv[1];tree=p/kind
strict=kind in ['strict','asan'];asan=kind=='asan'
flags=['-DLUA_USE_ASSERT','-DLJ_FUNC_TEST_HELPERS','-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_XSAVE_TEST_HELPERS'] if strict else []
env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';'+str(tree/'tests/lib/?.lua')+';;'
if asan:env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
else:env.pop('ASAN_OPTIONS',None)
rows=[]
def run(name,cmd,bound=30,cwd=tree):
 start=time.monotonic()
 try:
  r=subprocess.run(cmd,cwd=cwd,env=env,capture_output=True,text=True,timeout=bound)
  result=dict(exit=r.returncode,stdout=r.stdout,stderr=r.stderr)
 except subprocess.TimeoutExpired as e:
  result=dict(exit=None,timeout=True,stdout=(e.stdout or b'').decode(errors='replace'),stderr=(e.stderr or b'').decode(errors='replace'))
 row=dict(name=name,command=cmd,cwd=str(cwd),seconds=time.monotonic()-start,environment={k:env[k] for k in ['LUA_PATH','ASAN_OPTIONS'] if k in env},**result)
 rows.append(row);(p/(kind+'-build-and-reducer.json')).write_text(json.dumps(rows,indent=2)+'\n')
 print(kind,name,'exit',result['exit'],flush=True)
 return result
cmd=['taskset','-c','0-15','make','-C',str(tree/'src'),'-j4']
if strict:cmd+=['BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:','XCFLAGS='+' '.join(flags)]
if asan:cmd+=['CC=clang','CCOPT=-O1','TARGET_CFLAGS=-fsanitize=address -fno-omit-frame-pointer','TARGET_LDFLAGS=-fsanitize=address']
r=run('build',cmd,240);assert r['exit']==0,r['stderr']
(p/(kind+'-binaries.json')).write_text(json.dumps({f:hashlib.sha256((tree/'src'/f).read_bytes()).hexdigest() for f in ['luajit','libluajit.a']},indent=2)+'\n')
if asan:
 r=run('host-instrumentation',['nm',str(tree/'src/host/minilua'),str(tree/'src/host/buildvm')]);assert r['exit']==0 and '__asan_' not in r['stdout']
 r=run('runtime-instrumentation',['nm',str(tree/'src/lj_record.o')]);assert r['exit']==0 and '__asan_' in r['stdout']
for mode in ['-joff','-jon']:
 r=run('reducer'+mode,['taskset','-c','0-15',str(tree/'src/luajit'),mode,str(p/'clib-method-reducer.lua')])
 assert r['exit']==(1 if kind=='baseline' and mode=='-jon' else 0),r
