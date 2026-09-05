from pathlib import Path
import os, subprocess, signal, json, time, hashlib, resource, re
p=Path(__file__).parent
source=Path('/tmp/lj-gc-coalescing-final-20260905-0eig4rlf')
repo=Path('/workspaces/lj-lockless')
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
results=[]
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
normal=source/'normal'
metadata={'source_tree':str(source),'production_commit':'d680421c','normal_binary_sha256':sha(normal/'src/luajit'),'assert_archive_sha256':sha(source/'assert/src/libluajit.a'),'tests':{},'purpose':'Broader Linux concurrency stability check of landed coalescing/JIT code. No pending allocator/scalar changes.'}
for f in subprocess.check_output(['git','ls-tree','-r','--name-only','d680421c','src'],cwd=repo,text=True).splitlines():
 q=normal/f
 if q.is_file():
  raw=subprocess.check_output(['git','show','d680421c:'+f],cwd=repo)
  assert q.read_bytes()==raw,f
metadata['all_tracked_production_files_match']=True
(p/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
def record(r):
 results.append(r);(p/'results.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(r),flush=True)
def run(name,cmd,limit,cwd,overrides=None):
 env=os.environ.copy()
 for key in list(env):
  if key.startswith('LJ_M5_'): del env[key]
 env['LUA_PATH']=str(normal/'src/?.lua')+';'+str(normal/'tests/lib/?.lua')+';;'
 if overrides: env.update(overrides)
 start=time.monotonic()
 with (p/(name+'.stdout')).open('w') as out,(p/(name+'.stderr')).open('w') as err:
  proc=subprocess.Popen(cmd,cwd=cwd,env=env,stdout=out,stderr=err,start_new_session=True)
  try:proc.wait(timeout=limit);status='complete'
  except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait();status='timeout'
 record({'name':name,'command':cmd,'cwd':str(cwd),'env_overrides':overrides or {},'lua_path':env['LUA_PATH'],'seconds':time.monotonic()-start,'limit_seconds':limit,'status':status,'exit':proc.returncode,'stdout':(p/(name+'.stdout')).read_text()[:1000],'stderr':(p/(name+'.stderr')).read_text()[:1000]})
for fixture in ['t-tab-rooted-get-try','t-tab-rooted-len-try']:
 root=source/'assert';f=root/'tests'/(fixture+'.c');metadata['tests'][str(f.relative_to(root))]=sha(f)
 cmd=['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-mcx16','-DLJ_GC2_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLUA_USE_ASSERT','-I'+str(root/'src'),str(f),str(root/'src/libluajit.a'),'-lm','-ldl','-pthread']
 cmd+=['-Wl,--wrap='+s for s in dict.fromkeys(re.findall(r'__wrap_(\w+)\s*\(',f.read_text()))]+['-o',str(p/fixture)]
 build=subprocess.run(cmd,text=True,capture_output=True)
 record({'name':fixture+'-compile','command':cmd,'exit':build.returncode,'output':build.stdout+build.stderr})
 if not build.returncode:run(fixture,['taskset','-c','0-15',str(p/fixture)],30,root)
cases=[('resize-general','t-tab-resize-stress.lua','-joff',30,{'LJ_M5_TAB_RESIZE_STRESS_CASES':'weak,gcmark,gckey,weakkey,weakmeta,finalizer,metatable,len,traversal,nextchurn,nextinvalid,tableclear,tablelib,tablelibshift,metadispatch'}),('resize-jit','t-tab-resize-stress.lua','-jon',30,{'LJ_M5_TAB_RESIZE_STRESS_CASES':'jitstore,jitread,jititer'}),('resize-weakfinjit','t-tab-resize-stress.lua','-jon',20,{'LJ_M5_TAB_RESIZE_STRESS_CASES':'weakfinjit'}),('resize-remote-stack','t-tab-resize-stress.lua','-jon',20,{'LJ_M5_TAB_RESIZE_STRESS_CASES':'remotejitgc'}),('strtab-gc','t-strtab-gc-stress.lua','-joff',30,{}),('buffer-joff','t-buffer-thread-safety.lua','-joff',30,{}),('buffer-jon','t-buffer-thread-safety.lua','-jon',30,{}),('jit-trace-pressure','t-jit-trace-gc-pressure.lua','-jon',20,{})]
for name,f,mode,limit,env in cases:
 fixture=normal/'tests'/f;metadata['tests']['tests/'+f]=sha(fixture)
 run(name,['taskset','-c','0-15',str(normal/'src/luajit'),mode,str(fixture)],limit,normal,env)
(p/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
