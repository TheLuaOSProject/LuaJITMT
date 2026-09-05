from pathlib import Path
import hashlib, json, os, resource, subprocess, time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent
repo=Path('/workspaces/lj-lockless')
runner=Path('/tmp/lj-clib-cdata-combined-20260905-bxrxos7h/strict/src/luajit')
tmp=p/'canonical-tmp'
tmp.mkdir(exist_ok=True)
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
env=os.environ.copy()
env.update(LJ_TEST_ROOT=str(repo),JOBS='4',MAKE_JOBS='4',TMPDIR=str(tmp),LUA_PATH=str(repo/'src/?.lua')+';;')
env.pop('ASAN_OPTIONS',None)
source_inputs=json.loads(Path('/tmp/lj-reclaim-fair-combined-20260905-yws2eaap/setup.json').read_text())['combined_inputs']
for f,h in source_inputs.items():assert sha(repo/f)==h,f
cmd=['taskset','-c','0-15',str(runner),str(repo/'tools/test.lua'),'m3_gc2_worker_scheduler']
row=dict(command=cmd,cwd=str(repo),expected_runtime_processes=3,runner_sha256=sha(runner),
         environment={k:env[k] for k in ['LJ_TEST_ROOT','JOBS','MAKE_JOBS','TMPDIR','LUA_PATH']},
         inputs={f:sha(repo/f) for f in ['tests/t-gc2-worker-scheduler.c','tests/t-gc-workers.lua','tests/suites/m3_gc.lua']})
started=time.monotonic()
with (p/'canonical.stdout').open('wb') as so,(p/'canonical.stderr').open('wb') as se:
    try:row['exit']=subprocess.run(cmd,cwd=repo,env=env,stdout=so,stderr=se,timeout=240).returncode
    except subprocess.TimeoutExpired:row['exit'],row['timed_out']=124,True
row['seconds']=time.monotonic()-started
row['binaries']={f:sha(repo/'src'/f) for f in ['luajit','libluajit.a','libluajit.so']}
row['temporary_files']={str(f.relative_to(p)):sha(f) for f in tmp.rglob('*') if f.is_file()}
for f,h in source_inputs.items():assert sha(repo/f)==h,f
(p/'canonical.json').write_text(json.dumps(row,indent=2)+'\n')
print('canonical',row['exit'],row['seconds'],flush=True)
assert row['exit']==0
