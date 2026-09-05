from pathlib import Path
import hashlib, json, os, resource, shutil, subprocess, time

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
p = Path(__file__).resolve().parent
root = Path('/workspaces/lj-lockless')
inputs = json.loads((p/'inputs.json').read_text())
tree = Path(inputs['runtime'])
sha = lambda f: hashlib.sha256(Path(f).read_bytes()).hexdigest()
src = p/'t-tab-scalar-hit.c'
assert not src.exists()
shutil.copy2(root/'tests/t-tab-scalar-hit.c', src)
assert sha(src) == inputs['references'][str(root/'tests/t-tab-scalar-hit.c')]
binary = p/'scalar-control'
deps = p/'scalar-control.d'
out = p/'primitive-control-results.json'
assert not out.exists()
flags = ['-DLUA_USE_APICHECK', '-DLUA_USE_ASSERT', '-DLJ_FUNC_TEST_HELPERS',
         '-DLJ_GC2_TEST_HELPERS', '-DLJ_TAB_TEST_HELPERS', '-DLJ_ARENA_TEST_HELPERS',
         '-DLJ_TRACE_TEST_HELPERS', '-DLJ_XSAVE_TEST_HELPERS']
env = {'LUA_PATH': str(tree/'src/?.lua')+';;'}
rows = []
def run(cmd, stage, source_inputs):
    start = time.monotonic()
    try:
        q = subprocess.run(cmd, cwd=p, env={**os.environ, **env},
                           capture_output=True, text=True, timeout=20)
        result = {'exit': q.returncode, 'stdout': q.stdout, 'stderr': q.stderr}
    except subprocess.TimeoutExpired as e:
        dec = lambda v: v.decode(errors='replace') if isinstance(v, bytes) else v or ''
        result = {'exit': None, 'timeout': True, 'stdout': dec(e.stdout), 'stderr': dec(e.stderr)}
    row = {'command': cmd, 'cwd': str(p), 'environment': env,
           'stage': stage, 'seconds': time.monotonic()-start, **result,
           'inputs': {str(f): sha(f) for f in source_inputs}}
    rows.append(row)
    out.write_text(json.dumps(rows, indent=2)+'\n')
    print(stage, result['exit'], result['stdout'], result['stderr'], flush=True)
    return row
cmd = ['cc', '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-mcx16']+flags
cmd += ['-MMD', '-MF', str(deps), '-I'+str(tree/'src'), str(src),
        str(tree/'src/libluajit.a'), '-lm', '-ldl', '-pthread', '-o', str(binary)]
row = run(cmd, 'compile', [p/'run-primitive-control.py', p/'inputs.json', src, tree/'src/libluajit.a'])
if row['exit'] == 0:
    row['binary_sha256'] = sha(binary)
    row['transitive_dependencies'] = {f: sha(f) for f in deps.read_text().replace('\\\n', ' ').split(': ', 1)[1].split()}
    out.write_text(json.dumps(rows, indent=2)+'\n')
    run(['taskset', '-c', '24-25', str(binary), 'paused-only'], 'run',
        [src, binary, tree/'src/libluajit.a'])
