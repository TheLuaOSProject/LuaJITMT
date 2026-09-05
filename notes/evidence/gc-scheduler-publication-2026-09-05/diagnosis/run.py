from pathlib import Path
import subprocess, os, time, json, hashlib, resource

r = Path(__file__).resolve().parent
old = Path('/tmp/lj-gc-auto-admission-20260905-h7ntx71p')
prior = Path('/tmp/lj-gc-auto-stop-overlap-20260905-y4h4cc8a')
trees = {'control': prior/'controlextrahelpers', 'candidate': old/'extrahelpers',
         'veto': prior/'extrahelpers'}
flags = ['-DLUA_USE_ASSERT', '-DLUA_USE_APICHECK', '-DLJ_GC2_TEST_HELPERS',
         '-DLJ_TRACE_TEST_HELPERS', '-DLJ_TAB_TEST_HELPERS', '-DLJ_FUNC_TEST_HELPERS',
         '-DLJ_UDATA_TEST_HELPERS', '-DLJ_STR_TEST_HELPERS']
rows = []
def identity(p):
    p = Path(p)
    b = p.read_bytes()
    return {'path': str(p), 'sha256': hashlib.sha256(b).hexdigest(), 'bytes': len(b)}

def run(v, name, argv, inputs, timeout=60):
    env = dict(os.environ, LUA_PATH=str(trees[v]/'src/?.lua')+';'+str(trees[v]/'tests/lib/?.lua')+';;')
    out = r/(name+'.stdout'); err = r/(name+'.stderr')
    st = time.monotonic()
    with out.open('w') as o, err.open('w') as e:
        try:
            p = subprocess.run(argv, cwd=r, env=env, stdout=o, stderr=e, timeout=timeout)
            code = p.returncode
        except subprocess.TimeoutExpired:
            code = 'timeout'
    row = {'name': name, 'variant': v, 'argv': argv, 'cwd': str(r),
           'LUA_PATH': env['LUA_PATH'], 'ASAN_OPTIONS': env.get('ASAN_OPTIONS'),
           'timeout_seconds': timeout, 'exit_code': code, 'seconds': time.monotonic()-st,
           'inputs': [identity(p) for p in inputs], 'stdout': identity(out), 'stderr': identity(err)}
    rows.append(row)
    (r/'results.json').write_text(json.dumps(rows, indent=2)+'\n')
    print(name, code, round(row['seconds'], 4), flush=True)
    return code

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
fixture = r/'diagnostic/tests/t-gc2-worker-scheduler.c'
ready = []
for v, tree in trees.items():
    archive = tree/'src/libluajit.a'; exe = r/(v+'-diagnostic')
    expected = json.loads((r/'inputs.json').read_text())[v]['src/libluajit.a']['sha256']
    assert identity(archive)['sha256'] == expected
    argv = ['cc', '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
            '-I'+str(tree/'src'), *flags, str(fixture), str(archive), '-Wl,-E',
            '-lm', '-ldl', '-pthread', '-Wl,--wrap=pthread_create',
            '-Wl,--wrap=pthread_join', '-o', str(exe)]
    if run(v, v+'-compile', argv, [fixture, archive]) == 0:
        ready.append(v)

# Fixed bounded observations, using unpinned fresh processes and no background
# load generation. Diagnostic code is reached only after the original bound.
for i in range(20):
    for v in ready:
        exe = r/(v+'-diagnostic')
        run(v, v+'-'+str(i).zfill(2), [str(exe)], [exe, fixture, trees[v]/'src/libluajit.a'])
