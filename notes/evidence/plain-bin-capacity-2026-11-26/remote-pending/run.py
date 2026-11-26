import hashlib, json, os, pathlib, resource, subprocess, time

root = pathlib.Path(__file__).resolve().parent
base = pathlib.Path('/tmp/lj-plain-shrink-final-20261126-lxy_jr_l/asan')
cand = pathlib.Path('/tmp/lj-plain-bin-capacity-20261126-wyqryfj8/asan')
expected = {
    'baseline': '1d7468f5a975d1255d8ea69ca72beb284a5e9a85f59046a058c9919fcf822ed5',
    'candidate': '1a3420fe54b2450bedfeb0bca775f2625e6452459ac1da94598eac5a4acbfe29',
}
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
rows = []
identities = {}
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1')
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def run(label, cmd):
    start = time.monotonic()
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=40, env=env)
    (root / (label + '.stdout')).write_bytes(p.stdout)
    (root / (label + '.stderr')).write_bytes(p.stderr)
    row = dict(label=label, command=cmd, exit=p.returncode,
               seconds=time.monotonic()-start)
    rows.append(row)
    (root/'results.json').write_text(json.dumps(rows, indent=2)+'\n')
    print(json.dumps(row), flush=True)
    return p.returncode
for name, tree in [('baseline', base), ('candidate', cand)]:
    arena = tree/'src/lj_arena.c'
    assert sha(arena) == expected[name]
    inputs = [root/'witness.c', tree/'tests/t-arena-tls-stub.c',
              arena, tree/'src/lj_prng.c', tree/'src/lj_assert.c']
    identities[name] = {str(p): sha(p) for p in inputs}
    cmd = ['taskset', '-c', '9', 'clang', '-std=gnu11', '-O2', '-g',
           '-Wall', '-Wextra', '-Werror', '-mcx16', '-DLUA_USE_ASSERT',
           '-DLJ_BUF_TEST_HELPERS', '-DLJ_GC2_TEST_HELPERS',
           '-DLJ_ARENA_TEST_HELPERS', '-fsanitize=address',
           '-fno-omit-frame-pointer', '-DLUAJIT_SECURITY_PRNG=0',
           '-I'+str(tree/'src'), '-I'+str(tree/'tests')]
    cmd += list(map(str, inputs))
    cmd += ['-fsanitize=address', '-lm', '-ldl', '-pthread', '-o', str(root/name)]
    assert run(name+'-build', cmd) == 0
    identities[name]['binary_sha256'] = sha(root/name)
    assert run(name+'-positive', ['taskset','-c','9',str(root/name)]) == 0
    assert run(name+'-original-assertion', ['taskset','-c','9',str(root/name),
                                          '--require-eight']) == -6
(root/'identities.json').write_text(json.dumps(identities,indent=2)+'\n')
