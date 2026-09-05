from pathlib import Path
import hashlib, json, os, resource, subprocess, sys, time

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
p = Path(__file__).resolve().parent
sha = lambda f: hashlib.sha256(Path(f).read_bytes()).hexdigest()
variant, source_name, output_name, *args = sys.argv[1:]
assert variant in ['candidate', 'optimized', 'asan', 'baseline']
tree = p/variant if variant != 'baseline' else Path('/tmp/lj-clib-cdata-combined-20260905-bxrxos7h/strict')
src = p/'fixtures'/source_name
folder = p/'validation'/output_name
folder.mkdir(parents=True, exist_ok=False)
binary = folder/'fixture'
flags = ['-DLJ_GC2_TEST_HELPERS'] if variant == 'optimized' else [
    '-DLUA_USE_APICHECK', '-DLUA_USE_ASSERT', '-DLJ_FUNC_TEST_HELPERS',
    '-DLJ_GC2_TEST_HELPERS', '-DLJ_TAB_TEST_HELPERS', '-DLJ_ARENA_TEST_HELPERS',
    '-DLJ_TRACE_TEST_HELPERS', '-DLJ_XSAVE_TEST_HELPERS']
cmd = ['clang' if variant == 'asan' else 'cc', '-std=gnu11',
       '-O1' if variant == 'asan' else '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-mcx16'] + flags
if variant == 'asan': cmd += ['-fsanitize=address', '-fno-omit-frame-pointer']
cmd += ['-MMD', '-MF', str(folder/'deps.d'), '-I'+str(tree/'src'), '-I'+str(p/'fixtures'),
        str(src), str(tree/'src/libluajit.a'), '-lm', '-ldl', '-pthread', '-o', str(binary)]
inputs = {str(f): sha(f) for f in [p/'validation-driver.py', src, tree/'src/libluajit.a',
                                  p/'source-identity.json']}
start = time.monotonic()
q = subprocess.run(cmd, cwd=p, capture_output=True, text=True, timeout=40)
record = {'command': cmd, 'cwd': str(p), 'seconds': time.monotonic()-start,
          'exit': q.returncode, 'stdout': q.stdout, 'stderr': q.stderr, 'inputs': inputs}
if q.returncode == 0:
    record['binary_sha256'] = sha(binary)
    record['transitive_dependencies'] = {f: sha(f) for f in (folder/'deps.d').read_text().replace('\\\n', ' ').split(': ', 1)[1].split()}
(folder/'compile.json').write_text(json.dumps(record, indent=2)+'\n')
print(output_name, 'compile', q.returncode, flush=True)
assert q.returncode == 0, q.stderr
inputs[str(binary)] = sha(binary)
env = {'LUA_PATH': str(tree/'src/?.lua')+';;'}
if variant == 'asan': env['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1'
commands = [a.split(',') for a in args] if args else [[]]
rows = []
for argv in commands:
    cmd = ['taskset', '-c', '24-25', str(binary)] + argv
    start = time.monotonic()
    try:
        q = subprocess.run(cmd, cwd=p, env={**os.environ, **env}, capture_output=True, text=True, timeout=20)
        result = {'exit': q.returncode, 'stdout': q.stdout, 'stderr': q.stderr}
    except subprocess.TimeoutExpired as e:
        dec = lambda v: v.decode(errors='replace') if isinstance(v, bytes) else v or ''
        result = {'exit': None, 'timeout': True, 'stdout': dec(e.stdout), 'stderr': dec(e.stderr)}
    row = {'command': cmd, 'cwd': str(p), 'environment': env, 'timeout_seconds': 20,
           'seconds': time.monotonic()-start, **result, 'inputs': inputs}
    rows.append(row)
    (folder/'results.json').write_text(json.dumps(rows, indent=2)+'\n')
    print(output_name, argv, 'exit', row['exit'], 'seconds', row['seconds'], flush=True)
