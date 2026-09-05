from pathlib import Path
import hashlib, json, subprocess, time

p = Path(__file__).resolve().parent
tree = p/'candidate'
src = p/'fixtures/t-jit-idle-reclaim-entry.c'
binary = p/'idle-candidate'
deps = p/'idle-candidate.d'
sha = lambda f: hashlib.sha256(Path(f).read_bytes()).hexdigest()
base = json.loads((p/'base-inputs.json').read_text())
assert sha(src) == base['fixtures']['fixtures/t-jit-idle-reclaim-entry.c']
out = p/'idle-compile.json'
assert not out.exists()
flags = ['-DLUA_USE_APICHECK', '-DLUA_USE_ASSERT', '-DLJ_FUNC_TEST_HELPERS',
         '-DLJ_GC2_TEST_HELPERS', '-DLJ_TAB_TEST_HELPERS', '-DLJ_ARENA_TEST_HELPERS',
         '-DLJ_TRACE_TEST_HELPERS', '-DLJ_XSAVE_TEST_HELPERS']
cmd = ['cc', '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-mcx16']+flags
cmd += ['-MMD', '-MF', str(deps), '-I'+str(tree/'src'), '-I'+str(p/'fixtures'),
        str(src), str(tree/'src/libluajit.a'), '-lm', '-ldl', '-pthread', '-o', str(binary)]
start = time.monotonic()
q = subprocess.run(cmd, cwd=p, capture_output=True, text=True, timeout=40)
record = {'command': cmd, 'cwd': str(p), 'seconds': time.monotonic()-start,
          'exit': q.returncode, 'stdout': q.stdout, 'stderr': q.stderr,
          'inputs': {str(f): sha(f) for f in [p/'prepare-idle-fixture.py', p/'source-identity.json',
                     src, p/'fixtures/lib/lua_fixture_helpers.h', tree/'src/libluajit.a']}}
if q.returncode == 0:
    record['binary_sha256'] = sha(binary)
    record['transitive_dependencies'] = {f: sha(f) for f in deps.read_text().replace('\\\n', ' ').split(': ', 1)[1].split()}
out.write_text(json.dumps(record, indent=2)+'\n')
print('compile exit', q.returncode, 'binary', record.get('binary_sha256'))
assert q.returncode == 0
