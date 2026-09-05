from pathlib import Path
import hashlib, json, os, resource, subprocess, sys, time

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
p = Path(__file__).resolve().parent
script = p/(sys.argv[1]+'.gdb')
out = p/(sys.argv[1]+'-results.json')
assert not out.exists(), out
original = json.loads((p/'inputs.json').read_text())
binary = Path(original['binary'])
sha = lambda f: hashlib.sha256(Path(f).read_bytes()).hexdigest()
assert sha(binary) == original['binary_sha256']
environment = {'LUA_PATH': original['runtime']+'/src/?.lua;;'}
cmd = ['taskset', '-c', '24-25', 'gdb', '-q', '-batch', '-x', str(script), '--args', str(binary)]
start = time.monotonic()
try:
    q = subprocess.run(cmd, cwd=p, env={**os.environ, **environment},
                       capture_output=True, text=True, timeout=12)
    result = {'exit': q.returncode, 'stdout': q.stdout, 'stderr': q.stderr}
except subprocess.TimeoutExpired as e:
    dec = lambda v: v.decode(errors='replace') if isinstance(v, bytes) else v or ''
    result = {'exit': None, 'timeout': True, 'stdout': dec(e.stdout), 'stderr': dec(e.stderr)}
record = {'command': cmd, 'cwd': str(p), 'environment': environment,
          'seconds': time.monotonic()-start, **result,
          'inputs': {str(f): sha(f) for f in [p/'run-observer.py', p/'inputs.json', script, binary]},
          'note': 'Read-only debugger observation; quit terminates the still-paused target. Never a complete fixture pass.'}
out.write_text(json.dumps(record, indent=2)+'\n')
print(record['stdout'])
print(record['stderr'])
print('observer exit', record['exit'])
