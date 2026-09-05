from pathlib import Path
import hashlib, json, os, resource, subprocess, time
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
p = Path(__file__).resolve().parent
repo = Path('/workspaces/lj-lockless')
runner = Path('/tmp/lj-clib-cdata-combined-20260905-bxrxos7h/strict/src/luajit')
tmp = p/'canonical-tmp'
tmp.mkdir(exist_ok=True)
env = os.environ.copy()
env.update(LJ_TEST_ROOT=str(repo), JOBS='4', MAKE_JOBS='4', TMPDIR=str(tmp),
           LUA_PATH=str(repo/'src/?.lua')+';;')
env.pop('ASAN_OPTIONS', None)
rows = []
sha = lambda f: hashlib.sha256(f.read_bytes()).hexdigest()
setup = json.loads((p/'setup.json').read_text())
fixtures = ['tests/t-func-construction-anchor.c', 'tests/t-gc2-constructor-defer.c',
            'tests/t-gc2-constructor-mixed.c', 'tests/t-gc2-constructor-fairness.c',
            'tests/suites/m3_gc.lua', 'tests/suites/m5_runtime.lua']
for name, count in [('m5_function_construction_anchors', 1),
                    ('m3_gc2_auto_control', 37), ('m3_gc2_constructor_defer', 11)]:
    for f, expected in setup['combined_inputs'].items():
        assert sha(repo/f) == expected, f
    cmd = ['taskset','-c','0-15',str(runner),str(repo/'tools/test.lua'),name]
    row = dict(name=name, command=cmd, cwd=str(repo), expected_runtime_processes=count,
               environment={k:env[k] for k in ['LJ_TEST_ROOT','JOBS','MAKE_JOBS','TMPDIR','LUA_PATH']},
               runner_sha256=sha(runner), inputs={f:sha(repo/f) for f in fixtures})
    started=time.monotonic()
    with (p/(name+'.stdout')).open('wb') as so, (p/(name+'.stderr')).open('wb') as se:
        try:
            row['exit'] = subprocess.run(cmd,cwd=repo,env=env,stdout=so,stderr=se,timeout=240).returncode
        except subprocess.TimeoutExpired:
            row['exit'],row['timed_out']=124,True
    row['seconds']=time.monotonic()-started
    row['binaries']={f:sha(repo/'src'/f) for f in ['luajit','libluajit.a','libluajit.so'] if (repo/'src'/f).exists()}
    row['temporary_files']={str(f.relative_to(p)):sha(f) for f in tmp.rglob('*') if f.is_file()}
    for f, expected in setup['combined_inputs'].items():
        assert sha(repo/f) == expected, f
    rows.append(row)
    (p/'canonical.json').write_text(json.dumps(rows,indent=2)+'\n')
    print(name,row['exit'],row['seconds'],flush=True)
assert all(row['exit']==0 for row in rows)
