import hashlib, json, os, pathlib, resource, subprocess, time

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
p = pathlib.Path(__file__).parent
r = pathlib.Path('/workspaces/lj-lockless')
runner = pathlib.Path('/tmp/lj-clib-cache-root-20260905-i59mqoic/v2/strict/src/luajit')
sha = lambda q: hashlib.sha256(q.read_bytes()).hexdigest()
inputs = {n: sha(r/n) for n in ['tests/t-gc2-worker-scheduler.c',
          'tests/suites/m3_gc.lua', 'tests/t-gc-workers.lua', 'src/lj_gc2.c',
          'src/lj_gc.c', 'src/lj_tg.c', 'src/lj_safepoint.c', 'src/lj_obj.h']}
(p/'canonical-tmp').mkdir()
envadd = {'LJ_TEST_ROOT': str(r), 'JOBS': '4', 'MAKE_JOBS': '4',
          'TMPDIR': str(p/'canonical-tmp'), 'LUA_PATH': str(r/'src/?.lua')+';;'}
cmd = ['taskset', '-c', '0-15', str(runner), str(r/'tools/test.lua'),
       'm3_gc2_worker_scheduler']
start = time.monotonic()
with (p/'canonical.stdout').open('w') as out, (p/'canonical.stderr').open('w') as err:
    proc = subprocess.run(cmd, cwd=r, env={**os.environ, **envadd}, stdout=out,
                          stderr=err, timeout=180)
result = {'command': cmd, 'cwd': str(r), 'environment': envadd,
          'runner_sha256': sha(runner), 'sources': inputs, 'exit': proc.returncode,
          'seconds': time.monotonic()-start, 'expected_runtime_processes': 3,
          'default_binaries': {n: sha(r/'src'/n) for n in
                               ['luajit', 'libluajit.a', 'libluajit.so']},
          'temporary_files': {str(q.relative_to(p)): sha(q)
                              for q in (p/'canonical-tmp').rglob('*') if q.is_file()}}
(p/'canonical.json').write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(result), flush=True)
raise SystemExit(proc.returncode)
