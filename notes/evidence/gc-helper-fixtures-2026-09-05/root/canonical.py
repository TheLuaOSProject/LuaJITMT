from pathlib import Path
import hashlib,json,os,resource,subprocess,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent;r=Path('/workspaces/lj-lockless')
runner=Path('/tmp/lj-clib-cdata-combined-20260905-bxrxos7h/strict/src/luajit')
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
(p/'canonical-tmp').mkdir()
envadd={'LJ_TEST_ROOT':str(r),'JOBS':'4','MAKE_JOBS':'4','TMPDIR':str(p/'canonical-tmp'),'LUA_PATH':str(r/'src/?.lua')+';;'}
rows=[]
for name,count in [('m6_jit_alloc_account',5),('m10_generational',3)]:
 cmd=['taskset','-c','0-15',str(runner),str(r/'tools/test.lua'),name]
 inputs={n:sha(r/n) for n in ['tests/t-gc2-interp-hard-check.c','tests/t-gc2-alloc-account.c','tests/suites/m6_jit.lua','tests/suites/m9_m10_gc.lua']}
 start=time.monotonic()
 with (p/(name+'.stdout')).open('w') as out,(p/(name+'.stderr')).open('w') as err:
  try:
   q=subprocess.run(cmd,cwd=r,env={**os.environ,**envadd},stdout=out,stderr=err,timeout=240);result={'exit':q.returncode}
  except subprocess.TimeoutExpired:result={'exit':None,'timeout':True}
 row={'name':name,'command':cmd,'cwd':str(r),'environment':envadd,'expected_runtime_processes':count,'inputs':inputs,'runner_sha256':sha(runner),'seconds':time.monotonic()-start,'binaries':{n:sha(r/'src'/n) for n in ['luajit','libluajit.a','libluajit.so']},'temporary_files':{str(f.relative_to(p)):sha(f) for f in (p/'canonical-tmp').rglob('*') if f.is_file()},**result}
 rows.append(row);(p/'canonical.json').write_text(json.dumps(rows,indent=2)+'\n');print(name,result,flush=True)
assert all(x['exit']==0 for x in rows)
