from pathlib import Path
import hashlib,json,os,resource,subprocess,time

resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent
r=Path('/workspaces/lj-lockless')
tmp=p/'canonical-tmp'
tmp.mkdir(exist_ok=False)
runner=Path('/tmp/lj-clib-cdata-combined-20260905-bxrxos7h/strict/src/luajit')
env=os.environ.copy()
env.update(LJ_TEST_ROOT=str(r),JOBS='4',MAKE_JOBS='4',TMPDIR=str(tmp),LUA_PATH=str(r/'src/?.lua')+';;')
env.pop('ASAN_OPTIONS',None)
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
inputs=json.loads((p/'setup.json').read_text())['candidate_inputs']
fixture_names=['t-gc2-traverse.c']
rows=[]
for name,count in [('m3_gc2_weak_helper_claim',1)]:
    for f,digest in inputs.items():assert sha(r/f)==digest,f
    cmd=['taskset','-c','0-15',str(runner),str(r/'tools/test.lua'),name]
    row=dict(name=name,command=cmd,cwd=str(r),expected_runtime_processes=count,
        environment={k:env[k] for k in ['LJ_TEST_ROOT','JOBS','MAKE_JOBS','TMPDIR','LUA_PATH']},
        runner_sha256=sha(runner),fixtures={f:sha(r/'tests'/f) for f in fixture_names},
        suites={f:sha(r/'tests/suites'/f) for f in ['m3_gc.lua']})
    start=time.monotonic()
    with (p/(name+'.stdout')).open('wb') as so,(p/(name+'.stderr')).open('wb') as se:
        try:row['exit']=subprocess.run(cmd,cwd=r,env=env,stdout=so,stderr=se,timeout=300).returncode
        except subprocess.TimeoutExpired:row['exit'],row['timed_out']=124,True
    row['seconds']=time.monotonic()-start
    row['runtime_binaries']={f:sha(r/'src'/f) for f in ['luajit','libluajit.a']}
    row['temporary_files']={str(f.relative_to(p)):sha(f) for f in tmp.rglob('*') if f.is_file()}
    for f,digest in inputs.items():assert sha(r/f)==digest,f
    rows.append(row)
    (p/'canonical.json').write_text(json.dumps(rows,indent=2)+'\n')
    print(name,row['exit'],row['seconds'],flush=True)
assert all(x['exit']==0 for x in rows)
