from pathlib import Path
import subprocess, os, time, json, hashlib, resource
r = Path(__file__).resolve().parent
t = Path('/tmp/lj-gc-auto-admission-20260905-h7ntx71p/helpers')
exe = t.parent/'helpers-safety-t-func-construction-anchor'
argv = [str(exe)]
env = dict(os.environ, LUA_PATH=str(t/'src/?.lua')+';;')
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
st = time.monotonic()
with (r/'four-helper-control.stdout').open('w') as o, (r/'four-helper-control.stderr').open('w') as e:
    try:
        p = subprocess.run(argv, cwd=t, env=env, stdout=o, stderr=e, timeout=60)
        code = p.returncode
    except subprocess.TimeoutExpired:
        code = 'timeout'
row = {'argv': argv, 'cwd': str(t), 'exit_code': code, 'seconds': time.monotonic()-st,
       'timeout_seconds': 60, 'LUA_PATH': env['LUA_PATH'], 'identities': {}}
for p in [exe, t/'src/libluajit.a', t/'tests/t-func-construction-anchor.c']:
    b = p.read_bytes(); row['identities'][str(p)] = {'sha256': hashlib.sha256(b).hexdigest(), 'bytes': len(b)}
(r/'four-helper-control.json').write_text(json.dumps(row, indent=2)+'\n')
print(json.dumps({'exit_code': code, 'seconds': row['seconds']}), flush=True)
