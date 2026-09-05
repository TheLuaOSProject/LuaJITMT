import hashlib, json, os, pathlib, subprocess, time

p = pathlib.Path(__file__).parent
r = pathlib.Path('/workspaces/lj-lockless')
runner = pathlib.Path('/tmp/lj-udata-pure-receiver-combined-20260905-sn9vd57b/strict/src/luajit')
sha = lambda q: hashlib.sha256(q.read_bytes()).hexdigest()
inputs = {str(q.relative_to(r)): sha(q) for q in [r/'src/lj_opt_mem.c',
    r/'src/lj_crecord.c', r/'tests/suites/m6_jit.lua', r/'tests/t-jit-udata-pure.lua',
    r/'tests/t-jit-udata-pure-exclusions.lua']}
(p/'canonical-tmp').mkdir(exist_ok=True)
envadd = {'LJ_TEST_ROOT':str(r), 'JOBS':'4', 'MAKE_JOBS':'4',
          'TMPDIR':str(p/'canonical-tmp'), 'LUA_PATH':str(r/'src/?.lua')+';;'}
cmd = ['taskset', '-c', '0-15', str(runner), str(r/'tools/test.lua'), 'm6_jit_udata_pure']
start = time.monotonic()
with (p/'canonical.stdout').open('w') as out, (p/'canonical.stderr').open('w') as err:
    proc = subprocess.run(cmd, cwd=r, env={**os.environ, **envadd}, stdout=out,
                          stderr=err, timeout=180)
result = {'command':cmd, 'cwd':str(r), 'environment':envadd, 'runner_sha256':sha(runner),
          'sources':inputs, 'exit':proc.returncode, 'seconds':time.monotonic()-start,
          'default_binaries':{n:sha(r/'src'/n) for n in ['luajit','libluajit.a','libluajit.so']}}
(p/'canonical.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result),flush=True)
raise SystemExit(proc.returncode)
