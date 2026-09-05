from pathlib import Path
import hashlib, json, os, resource, subprocess, time

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
p = Path(__file__).resolve().parent
sha = lambda f: hashlib.sha256(Path(f).read_bytes()).hexdigest()
out = p/'idle-initial-results.json'
assert not out.exists()
cmd = ['taskset', '-c', '24-25', str(p/'idle-candidate')]
env = {'LUA_PATH': str(p/'candidate/src/?.lua')+';;'}
start = time.monotonic()
try:
    q = subprocess.run(cmd, cwd=p, env={**os.environ, **env},
                       capture_output=True, text=True, timeout=20)
    result = {'exit': q.returncode, 'stdout': q.stdout, 'stderr': q.stderr}
except subprocess.TimeoutExpired as e:
    dec = lambda v: v.decode(errors='replace') if isinstance(v, bytes) else v or ''
    result = {'exit': None, 'timeout': True, 'stdout': dec(e.stdout), 'stderr': dec(e.stderr)}
row = {'command': cmd, 'cwd': str(p), 'environment': env, 'timeout_seconds': 20,
       'seconds': time.monotonic()-start, **result,
       'inputs': {str(f): sha(f) for f in [p/'run-idle-initial.py', p/'source-identity.json',
          p/'fixtures/t-jit-idle-reclaim-entry.c', p/'idle-candidate', p/'candidate/src/libluajit.a']}}
out.write_text(json.dumps(row, indent=2)+'\n')
print(json.dumps(row, indent=2))
