from pathlib import Path
import os,re,json,subprocess,time,signal
p=Path(__file__).parent
results=[]
flags=['-DLJ_GC2_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLUA_USE_ASSERT']
def record(r):
 results.append(r);(p/'validation-results.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps({k:v for k,v in r.items() if k not in ['compile_command','command','compile_output']}),flush=True)
def run(name,cmd,limit=30,cwd=None,env=None):
 start=time.monotonic()
 with (p/(name+'.stdout')).open('w') as out,(p/(name+'.stderr')).open('w') as err:
  proc=subprocess.Popen(cmd,cwd=cwd,env=env,stdout=out,stderr=err,start_new_session=True)
  try:proc.wait(timeout=limit);status='complete'
  except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait();status='timeout'
 return {'name':name,'command':cmd,'exit':proc.returncode,'status':status,'limit_seconds':limit,'seconds':time.monotonic()-start,'stdout':(p/(name+'.stdout')).read_text()[:1000],'stderr':(p/(name+'.stderr')).read_text()[:1000]}
for kind,name in [('assert',n) for n in ['t-gc2-stats-arenas','t-arena-gcsweep','t-arena-empty-reclaimed-runtime','t-gc2-traverse','t-gc2-recovery','t-gc2-sweep-public-table-rescan','t-gc2-sweep-edge-lease','t-gc2-sweep-leaf-publication','t-gc2-sweep-table-coalescing','t-gc2-table-store-guard','t-gc2-public-store-weak-window','t-gc2-weak-resize-retry','t-jit-root-abort-retire','t-tab-scalar-hit','t-tab-rooted-get-try','t-tab-rooted-len-try','t-gc2-worker-scheduler','t-tg-terminal-orphan']]+[('normal','t-threading-lifecycle'),('normal','t-arena-state')]:
 root=p/kind;source=root/'tests'/(name+'.c');cmd=['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-mcx16']
 if kind=='assert':cmd+=flags
 cmd+=['-I'+str(root/'src'),str(source),str(root/'src/libluajit.a'),'-lm','-ldl','-pthread']
 cmd+=['-Wl,--wrap='+s for s in dict.fromkeys(re.findall(r'__wrap_(\w+)\s*\(',source.read_text()))]
 cmd+=['-o',str(p/name)]
 c=subprocess.run(cmd,capture_output=True,text=True)
 r={'name':name,'kind':kind,'compile_command':cmd,'compile_exit':c.returncode,'compile_output':c.stdout+c.stderr}
 if c.returncode==0:r.update(run(name,['taskset','-c','12' if name=='t-arena-state' else '0-15',str(p/name)],150 if name=='t-arena-state' else 60))
 record(r)
root=p/'normal';env=os.environ.copy();env['LUA_PATH']=str(root/'src/?.lua')+';;'
for mode in ['-joff','-jon']:
 record(run('stock-'+mode[1:],['taskset','-c','0-15',str(root/'src/luajit'),mode,'test.lua','--quiet'],120,str(root/'tests/stock/test'),env))

for name,fixture,mode,limit,overrides in [
 ('resize-general','t-tab-resize-stress.lua','-joff',30,{'LJ_M5_TAB_RESIZE_STRESS_CASES':'weak,gcmark,gckey,weakkey,weakmeta,finalizer,metatable,len,traversal,nextchurn,nextinvalid,tableclear,tablelib,tablelibshift,metadispatch'}),
 ('resize-jit','t-tab-resize-stress.lua','-jon',30,{'LJ_M5_TAB_RESIZE_STRESS_CASES':'jitstore,jitread,jititer'}),
 ('resize-weakfinjit','t-tab-resize-stress.lua','-jon',20,{'LJ_M5_TAB_RESIZE_STRESS_CASES':'weakfinjit'}),
 ('resize-remote-stack','t-tab-resize-stress.lua','-jon',20,{'LJ_M5_TAB_RESIZE_STRESS_CASES':'remotejitgc'}),
 ('strtab-gc','t-strtab-gc-stress.lua','-joff',30,{}),
 ('buffer-joff','t-buffer-thread-safety.lua','-joff',30,{}),
 ('buffer-jon','t-buffer-thread-safety.lua','-jon',30,{}),
 ('jit-trace-pressure','t-jit-trace-gc-pressure.lua','-jon',20,{})]:
 env=os.environ.copy()
 for k in list(env):
  if k.startswith('LJ_M5_'):del env[k]
 env['LUA_PATH']=str(root/'src/?.lua')+';'+str(root/'tests/lib/?.lua')+';;'
 env.update(overrides)
 r=run(name,['taskset','-c','0-15',str(root/'src/luajit'),mode,str(root/'tests'/fixture)],limit,root,env)
 r['env_overrides']=overrides
 r['lua_path']=env['LUA_PATH']
 record(r)
