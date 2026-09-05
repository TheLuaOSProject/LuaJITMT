from pathlib import Path
import hashlib, json, os, resource, subprocess, sys, time
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
p = Path(__file__).resolve().parent
kind = sys.argv[1]
tree = p / kind
inputs = json.loads((p/'setup.json').read_text())['combined_inputs']
for rel, expected in inputs.items():
    assert hashlib.sha256((tree/rel).read_bytes()).hexdigest() == expected, rel
strict, asan = kind != 'candidate', kind == 'asan'
flags = ['-DLUA_USE_APICHECK', '-DLUA_USE_ASSERT', '-DLJ_FUNC_TEST_HELPERS',
         '-DLJ_GC2_TEST_HELPERS', '-DLJ_TAB_TEST_HELPERS', '-DLJ_ARENA_TEST_HELPERS',
         '-DLJ_TRACE_TEST_HELPERS', '-DLJ_XSAVE_TEST_HELPERS',
         '-DLJ_UDATA_TEST_HELPERS', '-DLJ_STR_TEST_HELPERS'] if strict else []
env = os.environ.copy()
env['LUA_PATH'] = str(tree / 'src/?.lua') + ';' + str(tree / 'tests/lib/?.lua') + ';;'
if asan:
    env['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1'
else:
    env.pop('ASAN_OPTIONS', None)
cpus = {'candidate':'16-19', 'strict':'20-23', 'asan':'24-27'}[kind]
cmd = ['taskset', '-c', cpus, 'make', '-C', str(tree / 'src'), '-j4']
if strict:
    cmd += ['BUILDMODE=static', 'CCDEBUG=-g', 'TARGET_STRIP=:', 'XCFLAGS=' + ' '.join(flags)]
if asan:
    cmd += ['CC=clang', 'CCOPT=-O1',
            'TARGET_CFLAGS=-fsanitize=address -fno-omit-frame-pointer',
            'TARGET_LDFLAGS=-fsanitize=address']
start = time.monotonic()
r = subprocess.run(cmd, cwd=tree, env=env, capture_output=True, text=True, timeout=240)
(p / (kind + '-build.json')).write_text(json.dumps(dict(
    command=cmd, cwd=str(tree), flags=flags,
    environment={k: env[k] for k in ['LUA_PATH', 'ASAN_OPTIONS'] if k in env},
    seconds=time.monotonic()-start, exit=r.returncode, stdout=r.stdout, stderr=r.stderr), indent=2) + '\n')
assert r.returncode == 0, r.stderr
(p / (kind + '-binaries.json')).write_text(json.dumps({
    f: hashlib.sha256((tree / 'src' / f).read_bytes()).hexdigest()
    for f in ['luajit', 'libluajit.a']}, indent=2) + '\n')
if asan:
    checks = {}
    for name in ['host/minilua', 'host/buildvm', 'lj_gc.o', 'lj_gc2.o', 'lj_tab.o']:
        out = subprocess.check_output(['nm', '-u', str(tree / 'src' / name)], text=True)
        refs = [line for line in out.splitlines() if '__asan' in line]
        checks[name] = refs
        assert bool(refs) == (not name.startswith('host/')), name
    (p / 'asan-instrumentation.json').write_text(json.dumps(checks, indent=2) + '\n')
for rel, expected in inputs.items():
    assert hashlib.sha256((tree/rel).read_bytes()).hexdigest() == expected, rel
print(kind, 'built', flush=True)
