from pathlib import Path
import hashlib, json, subprocess, time

p = Path(__file__).resolve().parent
tree = p/'candidate'
identity = json.loads((p/'source-identity.json').read_text())
sha = lambda f: hashlib.sha256(Path(f).read_bytes()).hexdigest()
for name, hashes in identity['sources'].items():
    assert sha(tree/name) == hashes['candidate_sha256'], name
out = p/'candidate-build.json'
assert not out.exists(), out
flags = '-DLUA_USE_APICHECK -DLUA_USE_ASSERT -DLJ_FUNC_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLJ_XSAVE_TEST_HELPERS'
cmd = ['taskset', '-c', '16-19', 'make', '-C', str(tree/'src'), '-j4',
       'BUILDMODE=static', 'CCDEBUG=-g', 'TARGET_STRIP=:', 'XCFLAGS='+flags]
start = time.monotonic()
q = subprocess.run(cmd, cwd=tree, capture_output=True, text=True, timeout=180)
result = {'command': cmd, 'cwd': str(tree), 'seconds': time.monotonic()-start,
          'exit': q.returncode, 'stdout': q.stdout, 'stderr': q.stderr,
          'inputs': {str(f): sha(f) for f in [p/'build-candidate.py', p/'base-inputs.json', p/'source-identity.json']}}
if q.returncode == 0:
    result['binaries'] = {rel: sha(tree/rel) for rel in ['src/luajit', 'src/libluajit.a', 'src/jit/vmdef.lua']}
out.write_text(json.dumps(result, indent=2)+'\n')
for name, hashes in identity['sources'].items():
    assert sha(tree/name) == hashes['candidate_sha256'], name
print('build exit', q.returncode, 'seconds', result['seconds'], flush=True)
assert q.returncode == 0
