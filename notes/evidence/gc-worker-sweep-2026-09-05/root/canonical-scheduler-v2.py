from pathlib import Path
import hashlib, json, os, resource, subprocess, time

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
p = Path(__file__).resolve().parent
repo = Path('/workspaces/lj-lockless')
out = p / 'canonical-scheduler-v2'
out.mkdir(exist_ok=False)
tmp = out / 'tmp'
tmp.mkdir()
runner = Path('/tmp/lj-clib-cdata-combined-20260905-bxrxos7h/strict/src/luajit')
env = os.environ.copy()
env.update(LJ_TEST_ROOT=str(repo), JOBS='4', MAKE_JOBS='4', TMPDIR=str(tmp),
           LUA_PATH=str(repo/'src/?.lua')+';;')
env.pop('ASAN_OPTIONS', None)
sha = lambda f: hashlib.sha256(f.read_bytes()).hexdigest()
setup = json.loads((p/'setup.json').read_text())
inputs = setup['combined_inputs']
fixtures = ['tests/t-gc2-worker-scheduler.c', 'tests/suites/m3_gc.lua',
            'tests/t-gc2-worker-scheduler.lua']
for f, expected in inputs.items():
    assert sha(repo/f) == expected, f
cmd = ['taskset', '-c', '0-15', str(runner), str(repo/'tools/test.lua'),
       'm3_gc2_worker_scheduler']
row = dict(name='m3_gc2_worker_scheduler', command=cmd, cwd=str(repo),
           expected_runtime_processes=3,
           environment={k:env[k] for k in ['LJ_TEST_ROOT','JOBS','MAKE_JOBS','TMPDIR','LUA_PATH']},
           runner_sha256=sha(runner),
           inputs={f:sha(repo/f) for f in fixtures if (repo/f).is_file()},
           runtime_inputs_before=inputs,
           prior_failed_generation='canonical.json; no prior artifacts overwritten')
started = time.monotonic()
with (out/'stdout').open('wb') as so, (out/'stderr').open('wb') as se:
    try:
        row['exit'] = subprocess.run(cmd,cwd=repo,env=env,stdout=so,stderr=se,timeout=240).returncode
    except subprocess.TimeoutExpired:
        row['exit'],row['timed_out'] = 124,True
row['seconds'] = time.monotonic()-started
row['binaries'] = {f:sha(repo/'src'/f) for f in ['luajit','libluajit.a','libluajit.so']}
row['temporary_files'] = {str(f.relative_to(out)):sha(f) for f in tmp.rglob('*') if f.is_file()}
for f, expected in inputs.items():
    assert sha(repo/f) == expected, f
row['runtime_inputs_after_verified'] = len(inputs)
(out/'result.json').write_text(json.dumps(row,indent=2)+'\n')
print(row['name'],row['exit'],row['seconds'],flush=True)
assert row['exit'] == 0
