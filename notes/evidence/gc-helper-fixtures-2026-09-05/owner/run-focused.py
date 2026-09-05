from pathlib import Path
import hashlib, json, os, resource, subprocess, sys, time

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
p = Path(__file__).resolve().parent
generation, configuration = sys.argv[1:3]
configs = {
    '843-optimized-helpers': (p/'releasehelpers-v2', 'optimized'),
    '843-asan-control': (Path('/tmp/lj-gc-auto-control-root-20260905-au933s3w/asan'), 'asan'),
    '793-strict': (Path('/tmp/lj-clib-cdata-combined-20260905-bxrxos7h/strict'), 'strict'),
    '793-asan': (Path('/tmp/lj-clib-cdata-combined-20260905-bxrxos7h/asan'), 'asan'),
}
tree, variant = configs[configuration]
dest = p/generation
dest.mkdir(exist_ok=True)
out = dest/(configuration+'-results.json')
assert not out.exists(), out
sha = lambda f: hashlib.sha256(Path(f).read_bytes()).hexdigest()
envadd = {'LUA_PATH': str(tree/'src/?.lua')+';;'}
if variant == 'asan':
    envadd['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1'
flags = ['-DLJ_FUNC_TEST_HELPERS', '-DLJ_GC2_TEST_HELPERS', '-DLJ_TAB_TEST_HELPERS',
         '-DLJ_ARENA_TEST_HELPERS', '-DLJ_TRACE_TEST_HELPERS', '-DLJ_XSAVE_TEST_HELPERS']
if variant != 'optimized':
    flags = ['-DLUA_USE_APICHECK', '-DLUA_USE_ASSERT']+flags
rows = []

def run(cmd, meta, inputs, timeout=40):
    start = time.monotonic()
    try:
        q = subprocess.run(cmd, cwd=dest, env={**os.environ, **envadd},
                           capture_output=True, text=True, timeout=timeout)
        result = {'exit': q.returncode, 'stdout': q.stdout, 'stderr': q.stderr}
    except subprocess.TimeoutExpired as e:
        dec = lambda v: v.decode(errors='replace') if isinstance(v, bytes) else v or ''
        result = {'exit': None, 'timeout': True, 'stdout': dec(e.stdout), 'stderr': dec(e.stderr)}
    row = {'command': cmd, 'cwd': str(dest), 'environment': envadd,
           'inputs': {str(f): sha(f) for f in inputs},
           'seconds': time.monotonic()-start, **meta, **result}
    rows.append(row)
    out.write_text(json.dumps(rows, indent=2)+'\n')
    print(generation, configuration, meta, 'exit', result['exit'], flush=True)
    return row

for name in ['t-gc2-interp-hard-check', 't-gc2-alloc-account']:
    src = (p if generation == 'original' else p/'candidate-v4')/(name+'.c')
    binary = dest/(configuration+'-'+name)
    deps = dest/(configuration+'-'+name+'.d')
    cmd = ['clang' if variant == 'asan' else 'cc', '-std=gnu11',
           '-O1' if variant == 'asan' else '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-mcx16']+flags
    if variant == 'asan':
        cmd += ['-fsanitize=address', '-fno-omit-frame-pointer']
    cmd += ['-MMD', '-MF', str(deps), '-I'+str(tree/'src'), '-I'+str(p),
            str(src), str(tree/'src/libluajit.a'), '-lm', '-ldl', '-pthread', '-o', str(binary)]
    row = run(cmd, {'name': name, 'stage': 'compile'},
              [p/'run-focused.py', p/'focused-input-identity.json', src,
               p/'lib/lua_fixture_helpers.h', tree/'src/libluajit.a'], 60)
    if row['exit'] != 0:
        continue
    row['output_binary_sha256'] = sha(binary)
    headers = deps.read_text().replace('\\\n', ' ').split(': ', 1)[1].split()
    row['transitive_dependencies'] = {f: sha(f) for f in headers}
    out.write_text(json.dumps(rows, indent=2)+'\n')
    run(['taskset', '-c', '24', str(binary)], {'name': name, 'stage': 'run'},
        [src, binary, tree/'src/libluajit.a'])
