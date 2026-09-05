from pathlib import Path
import hashlib, json, subprocess, sys, time

p = Path(__file__).resolve().parent
variant = sys.argv[1]
assert variant in ['optimized', 'asan']
tree = p/variant
identity = json.loads((p/'source-identity.json').read_text())
sha = lambda f: hashlib.sha256(Path(f).read_bytes()).hexdigest()
for name, values in identity['sources'].items():
    assert sha(tree/name) == values['candidate_sha256'], name
out = p/(variant+'-build.json')
assert not out.exists()
flags = ['-DLJ_GC2_TEST_HELPERS'] if variant == 'optimized' else [
    '-DLUA_USE_APICHECK', '-DLUA_USE_ASSERT', '-DLJ_FUNC_TEST_HELPERS',
    '-DLJ_GC2_TEST_HELPERS', '-DLJ_TAB_TEST_HELPERS', '-DLJ_ARENA_TEST_HELPERS',
    '-DLJ_TRACE_TEST_HELPERS', '-DLJ_XSAVE_TEST_HELPERS']
cmd = ['taskset', '-c', '16-19' if variant == 'optimized' else '20-23',
       'make', '-C', str(tree/'src'), '-j4', 'BUILDMODE=static', 'CCDEBUG=-g',
       'TARGET_STRIP=:', 'XCFLAGS='+' '.join(flags)]
if variant == 'asan':
    cmd += ['CC=clang', 'CCOPT=-O1', 'TARGET_CFLAGS=-fsanitize=address -fno-omit-frame-pointer',
            'TARGET_LDFLAGS=-fsanitize=address']
start = time.monotonic()
q = subprocess.run(cmd, cwd=tree, capture_output=True, text=True, timeout=180)
record = {'command': cmd, 'cwd': str(tree), 'seconds': time.monotonic()-start,
          'exit': q.returncode, 'stdout': q.stdout, 'stderr': q.stderr,
          'inputs': {str(f): sha(f) for f in [p/'build-validation.py', p/'source-identity.json',
                                             p/'base-inputs.json']}}
if q.returncode == 0:
    record['binaries'] = {name: sha(tree/name) for name in ['src/luajit', 'src/libluajit.a', 'src/jit/vmdef.lua']}
out.write_text(json.dumps(record, indent=2)+'\n')
for name, values in identity['sources'].items():
    assert sha(tree/name) == values['candidate_sha256'], name
print(variant, 'build exit', q.returncode, 'seconds', record['seconds'], flush=True)
assert q.returncode == 0
