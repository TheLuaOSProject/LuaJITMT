from pathlib import Path
import os, subprocess, signal, time, json, hashlib, resource
r = Path(__file__).resolve().parent
t = r.parent/'debug-six'; s = t/'src'; fixture = t/'tests/t-func-construction-anchor.c'
flags = ['-DLUA_USE_ASSERT', '-DLUA_USE_APICHECK', '-DLJ_GC2_TEST_HELPERS',
         '-DLJ_TRACE_TEST_HELPERS', '-DLJ_TAB_TEST_HELPERS', '-DLJ_FUNC_TEST_HELPERS',
         '-DLJ_UDATA_TEST_HELPERS', '-DLJ_STR_TEST_HELPERS']
exe = r/'debug-six-fixture'
argv = ['cc', '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
        '-I'+str(s), *flags, str(fixture), str(s/'libluajit.a'),
        '-Wl,-E', '-lm', '-ldl', '-pthread', '-o', str(exe)]
rows = []
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
env = dict(os.environ, LUA_PATH=str(s/'?.lua')+';'+str(t/'tests/lib/?.lua')+';;')
st = time.monotonic()
with (r/'compile.stdout').open('w') as o, (r/'compile.stderr').open('w') as e:
    p = subprocess.run(argv, cwd=t, env=env, stdout=o, stderr=e)
rows.append({'argv': argv, 'exit_code': p.returncode, 'seconds': time.monotonic()-st,
             'stdout': 'compile.stdout', 'stderr': 'compile.stderr'})
if p.returncode == 0:
    argv = ['gdb', '--batch', '-x', str(r/'debug.gdb'), '--args', str(exe)]
    st = time.monotonic(); capture_time = None; interrupted = False
    with (r/'debug.stdout').open('w') as o, (r/'debug.stderr').open('w') as e:
        p = subprocess.Popen(argv, cwd=t, env=env, stdout=o, stderr=e)
        while p.poll() is None:
            text = (r/'debug.stdout').read_text()
            if capture_time is None and 'FUNC_DIAG COLLECT_ENTRY_CAPTURED' in text:
                capture_time = time.monotonic()
            if capture_time is not None and time.monotonic()-capture_time >= 3:
                p.send_signal(signal.SIGINT); interrupted = True; break
            if time.monotonic()-st > 15:
                p.send_signal(signal.SIGINT); interrupted = True; break
            time.sleep(.05)
        try: code = p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait(); code = 'debugger-timeout'
    rows.append({'argv': argv, 'exit_code': code, 'seconds': time.monotonic()-st,
                 'stdout': 'debug.stdout', 'stderr': 'debug.stderr',
                 'capture_seen': capture_time is not None, 'interrupted': interrupted,
                 'observation_seconds_after_capture': 3,
                 'note': 'Read-only debugger observation. A debugger exit 0 is not a fixture pass.'})
for row in rows:
    row['cwd'] = str(t); row['LUA_PATH'] = env['LUA_PATH']
    row['identities'] = {}
    for p in [exe, fixture, s/'libluajit.a', r/'inspect.py', r/'debug.gdb']:
        if p.exists():
            b = p.read_bytes(); row['identities'][str(p)] = {'sha256': hashlib.sha256(b).hexdigest(), 'bytes': len(b)}
(r/'debug-results.json').write_text(json.dumps(rows, indent=2)+'\n')
print(json.dumps([{k: x.get(k) for k in ['exit_code', 'seconds', 'capture_seen', 'interrupted']} for x in rows]), flush=True)
