from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import hashlib, json, os, resource, shutil, subprocess, time

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
p = Path(__file__).resolve().parent
root = Path('/workspaces/lj-lockless')
fixture = 'tests/t-arena-remote-pending.c'
update = p/'fixture-update'
update.mkdir(exist_ok=False)
def sha(f): return hashlib.sha256(Path(f).read_bytes()).hexdigest()
old = p/'gcc'/fixture
new = root/fixture
assert sha(new) == 'f72696bbe9df189a4508049d36ee9c274b0ba432fde90617c5c65d93ac3fabc8'
shutil.copy2(old, update/'original.c')
shutil.copy2(new, update/'t-arena-remote-pending.c')
(update/'metadata.json').write_text(json.dumps(dict(path=fixture,
    original_sha256=sha(old), corrected_sha256=sha(new),
    reason='Replace incorrect immediate-queue-count assumption with deterministic deferred lifetime and exact reuse checks.',
    diagnostic='/tmp/lj-remote-pending-deferral-20261126-vzwfb00s'), indent=2)+'\n')

def check(mode):
    tree = p/mode
    meta = json.loads((tree/'build.json').read_text())
    assert meta['exit'] == 0
    for rel, expected in meta['source_inputs'].items():
        assert sha(tree/rel) == expected, (mode, rel)
    out = tree/'check-pending'
    out.mkdir(exist_ok=False)
    sources = [update/'t-arena-remote-pending.c', tree/'tests/t-arena-tls-stub.c',
               tree/'src/lj_arena.c', tree/'src/lj_prng.c', tree/'src/lj_assert.c']
    identities = {str(f): sha(f) for f in sources}
    exe = out/'remote-pending'
    cmd = ['taskset', '-c', '0-7', meta['cc'], '-std=gnu11', '-O2', '-g',
           '-Wall', '-Wextra', '-Werror', '-mcx16', *meta['xcflags'], *meta['cflags'],
           '-DLUAJIT_SECURITY_PRNG=0', '-DLJ_ARENA_TEST_HELPERS',
           '-I'+str(tree/'src'), '-I'+str(tree/'tests'), *map(str, sources),
           *meta['ldflags'], '-lm', '-ldl', '-pthread', '-o', str(exe)]
    env = os.environ.copy()
    env.update(ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', TSAN_OPTIONS='halt_on_error=1')
    rows = []
    for name, argv, runtime in [('remote-pending-build', cmd, False),
            ('remote-pending', ['taskset', '-c', '0-7', str(exe)], True)]:
        t = time.monotonic()
        with (out/(name+'.stdout')).open('wb') as so, (out/(name+'.stderr')).open('wb') as se:
            try: code = subprocess.run(argv, cwd=tree, env=env, stdout=so, stderr=se, timeout=65).returncode
            except subprocess.TimeoutExpired: code = 124
        rows.append(dict(name=name, command=argv, seconds=time.monotonic()-t, exit=code, test=runtime))
        (out/'results.json').write_text(json.dumps(rows, indent=2)+'\n')
        print(mode, name, code, flush=True)
        assert code == 0, (mode, name, code)
        if not runtime: identities[str(exe)] = sha(exe)
    (out/'identities.json').write_text(json.dumps(identities, indent=2)+'\n')

with ThreadPoolExecutor(max_workers=3) as pool:
    list(pool.map(check, ['assert', 'asan', 'tsan']))

out = p/'canonical-pending'
out.mkdir(exist_ok=False)
meta = json.loads((p/'gcc/build.json').read_text())
src = {k:v for k,v in meta['source_inputs'].items() if k == 'Makefile' or k.startswith(('src/', 'dynasm/'))}
src[fixture] = sha(new)
(out/'inputs.json').write_text(json.dumps(src, indent=2)+'\n')
extra = {'LUA_PATH': str(p/'gcc/src/?.lua')+';;', 'JOBS': '6'}
env = os.environ.copy(); env.update(extra)
cmd = ['taskset', '-c', '0-7', str(p/'gcc/src/luajit'), 'tools/test.lua', 'm2_arena_publication']
t = time.monotonic()
with (out/'stdout').open('wb') as so, (out/'stderr').open('wb') as se:
    try: code = subprocess.run(cmd, cwd=root, env=env, stdout=so, stderr=se, timeout=120).returncode
    except subprocess.TimeoutExpired: code = 124
result = dict(command=cmd, exit=code, seconds=time.monotonic()-t, environment=extra,
    outputs={name:sha(root/'src'/name) for name in ['luajit','libluajit.a','libluajit.so']},
    source_mismatches=[k for k,v in src.items() if sha(root/k) != v])
(out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
print('canonical-pending', code, flush=True)
assert code == 0 and not result['source_mismatches']
