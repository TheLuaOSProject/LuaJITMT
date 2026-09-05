from pathlib import Path
import os, sys, subprocess, time, json, hashlib
r = Path(__file__).resolve().parent
v = sys.argv[1]
tree = r/v
s = tree/'src'
label = v+'-repaired-scheduler'+os.environ.get('SCHEDULER_RUN_SUFFIX', '')
exe = r/label
source = r/'t-gc2-worker-scheduler-repaired.c'
rows = []
env = dict(os.environ, LUA_PATH=str(s/'?.lua')+';'+str(tree/'tests/lib/?.lua')+';;')

def run(argv, name):
    st = time.monotonic()
    with (r/(label+'-'+name+'.stdout')).open('w') as out, (r/(label+'-'+name+'.stderr')).open('w') as err:
        try:
            code = subprocess.run(argv, cwd=tree, env=env, stdout=out, stderr=err, timeout=45).returncode
        except subprocess.TimeoutExpired:
            code = 'timeout'
    row = dict(argv=argv, name=name, cwd=str(tree), LUA_PATH=env['LUA_PATH'], exit_code=code, seconds=time.monotonic()-st, stdout=label+'-'+name+'.stdout', stderr=label+'-'+name+'.stderr')
    rows.append(row)
    (r/(label+'-results.json')).write_text(json.dumps(rows, indent=2)+'\n')
    print(json.dumps(row), flush=True)
    return code == 0

cmd = ['cc', '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-DLUA_USE_ASSERT', '-DLUA_USE_APICHECK', '-DLJ_GC2_TEST_HELPERS', '-DLJ_TRACE_TEST_HELPERS', '-DLJ_TAB_TEST_HELPERS', '-DLJ_FUNC_TEST_HELPERS', '-I'+str(s), '-I'+str(tree/'tests'), str(source), str(s/'libluajit.a'), '-Wl,-E', '-lm', '-ldl', '-pthread', '-Wl,--wrap=pthread_create', '-Wl,--wrap=pthread_join', '-o', str(exe)]
if run(cmd, 'compile'):
    run([str(exe)], 'run')
(r/(label+'-identities.json')).write_text(json.dumps({str(p.relative_to(r)): {'sha256': hashlib.sha256(p.read_bytes()).hexdigest(), 'bytes': p.stat().st_size} for p in [source, s/'libluajit.a', exe] if p.exists()}, indent=2)+'\n')
raise SystemExit(0 if all(row['exit_code']==0 for row in rows) else 1)
