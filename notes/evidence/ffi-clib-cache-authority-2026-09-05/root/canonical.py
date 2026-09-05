import hashlib, json, os, pathlib, resource, subprocess, time

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
p = pathlib.Path(__file__).parent
r = pathlib.Path('/workspaces/lj-lockless')
runner = p/'v2/strict/src/luajit'
sha = lambda q: hashlib.sha256(q.read_bytes()).hexdigest()
integration = json.loads((p/'integration.json').read_text())
names = list(integration['sources']) + list(integration['fixtures']) + ['tests/suites/m7_ffi.lua']
inputs = {n: sha(r/n) for n in names}
(p/'canonical-tmp').mkdir(exist_ok=True)
envadd = {'LJ_TEST_ROOT': str(r), 'JOBS': '4', 'MAKE_JOBS': '4',
          'TMPDIR': str(p/'canonical-tmp'), 'LUA_PATH': str(r/'src/?.lua')+';;'}
cmd = ['taskset', '-c', '0-15', str(runner), str(r/'tools/test.lua'),
       'm7_ffi_clib_cache_authority']
start = time.monotonic()
with (p/'canonical.stdout').open('w') as out, (p/'canonical.stderr').open('w') as err:
    proc = subprocess.run(cmd, cwd=r, env={**os.environ, **envadd}, stdout=out,
                          stderr=err, timeout=240)
result = {'command': cmd, 'cwd': str(r), 'environment': envadd,
          'runner_sha256': sha(runner), 'sources': inputs, 'exit': proc.returncode,
          'seconds': time.monotonic()-start, 'expected_runtime_processes': 153,
          'default_binaries': {n: sha(r/'src'/n) for n in
                               ['luajit', 'libluajit.a', 'libluajit.so']},
          'temporary_files': {str(q.relative_to(p)): sha(q)
                              for q in (p/'canonical-tmp').rglob('*') if q.is_file()}}
(p/'canonical.json').write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(result), flush=True)
raise SystemExit(proc.returncode)
