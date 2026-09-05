import hashlib, json, os, pathlib, resource, subprocess, time
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
p = pathlib.Path(__file__).parent
r = pathlib.Path('/workspaces/lj-lockless')
runner = p/'strict/src/luajit'
sha = lambda q: hashlib.sha256(q.read_bytes()).hexdigest()
inputs = json.loads((p/'source-identity.json').read_text())['candidate']['inputs']
assert all(sha(r/n) == h for n,h in inputs.items())
(p/'canonical-tmp').mkdir()
envadd = {'LJ_TEST_ROOT': str(r), 'JOBS': '4', 'MAKE_JOBS': '4',
          'TMPDIR': str(p/'canonical-tmp'), 'LUA_PATH': str(r/'src/?.lua')+';;'}
rows = []
for name, count in [('m3_gc2_auto_control', 37), ('m7_ffi_clib_cache_authority', 153), ('m7_ffi_clib_cdata_compare', 96)]:
    cmd = ['taskset', '-c', '0-15', str(runner), str(r/'tools/test.lua'), name]
    start = time.monotonic()
    with (p/(name+'.stdout')).open('w') as out, (p/(name+'.stderr')).open('w') as err:
        try:
            proc = subprocess.run(cmd, cwd=r, env={**os.environ, **envadd}, stdout=out,
                                  stderr=err, timeout=300)
            fields = {'exit': proc.returncode}
        except subprocess.TimeoutExpired:
            fields = {'exit': None, 'timeout': True}
    row = {'name':name, 'command': cmd, 'cwd': str(r), 'environment': envadd,
           'runner_sha256': sha(runner), 'seconds': time.monotonic()-start,
           'expected_runtime_processes': count, **fields,
           'default_binaries': {n: sha(r/'src'/n) for n in
                                ['luajit', 'libluajit.a', 'libluajit.so']},
           'temporary_files': {str(q.relative_to(p)): sha(q)
                               for q in (p/'canonical-tmp').rglob('*') if q.is_file()}}
    rows.append(row)
    (p/'canonical.json').write_text(json.dumps(rows, indent=2)+'\n')
    print(name,fields,flush=True)
    if fields['exit'] != 0:
        raise SystemExit(1)
assert all(sha(r/n) == h for n,h in inputs.items())
