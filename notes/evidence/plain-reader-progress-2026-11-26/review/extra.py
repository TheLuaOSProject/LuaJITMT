from pathlib import Path
import hashlib,json,os,resource,subprocess,sys,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent;root=Path('/workspaces/lj-lockless');mode=sys.argv[1];tree=p/mode;out=tree/'check-extra';out.mkdir(exist_ok=False)
meta=json.loads((tree/'build.json').read_text());assert meta['exit']==0
exe=tree/'src/luajit';assert hashlib.sha256(exe.read_bytes()).hexdigest()==meta['outputs']['luajit']
env=os.environ.copy();env.update(LUA_PATH=str(tree/'src/?.lua')+';'+str(tree/'tests/lib/?.lua')+';;',ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',TSAN_OPTIONS='halt_on_error=1')
rows=[]
def run(name,args,extra):
 e=env.copy();e.update(extra);cmd=['taskset','-c','0-7,9-15',str(exe),*args];t=time.monotonic()
 with (out/(name+'.stdout')).open('wb') as so,(out/(name+'.stderr')).open('wb') as se:
  try:code=subprocess.run(cmd,cwd=tree,env=e,stdout=so,stderr=se,timeout=120).returncode
  except subprocess.TimeoutExpired:code=124
 rows.append(dict(name=name,command=cmd,environment=extra,exit=code,seconds=time.monotonic()-t));(out/'results.json').write_text(json.dumps(rows,indent=2)+'\n');print(mode,name,code,flush=True)
if mode.startswith('nobuf'):
 run('disabled-buffer',['-e',"assert(not pcall(require,'string.buffer')); assert(('ab'..'cd')=='abcd'); assert(require('ffi').sizeof('int')==4)"],{})
elif mode.startswith(('noffi','nojit')):
 run('buffer-basic',['tests/t-buffer-thread-safety.lua'],{})
 run('buffer-public',['tests/t-buffer-concurrent-public.lua'],{'LJ_BUFFER_LARGE':'1'})
else:
 run('large-buffer',['tests/t-buffer-concurrent-public.lua'],{'LJ_BUFFER_LARGE':'1'})
 if not mode.startswith('tsan'):
  run('jit-flush-gc-workers',['tests/t-jit-flush-thread-stress.lua'],{'LJ_M6_JIT_FLUSH_THREAD_GC_WORKERS':'2'})
print(mode,'extra failures',sum(r['exit']!=0 for r in rows),flush=True);sys.exit(any(r['exit']!=0 for r in rows))
