from pathlib import Path
import sys, subprocess, os, time, json, hashlib

r = Path(__file__).resolve().parent
v = sys.argv[1]
tree = r/v
s = tree/'src'
label = v+'-existing-finalizers'
env = dict(os.environ, LUA_PATH=str(s/'?.lua')+';'+str(tree/'tests/lib/?.lua')+';;')
if v == 'asan':
    env['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1'
flags = ['-DLUA_USE_ASSERT', '-DLUA_USE_APICHECK'] if v in ('strict', 'asan') else []
if v == 'asan':
    flags += ['-fsanitize=address', '-fno-omit-frame-pointer']
rows = []
inputs = [s/'luajit', s/'libluajit.a']

def run(argv, name, limit=40):
    st = time.monotonic()
    with (r/(label+'-'+name+'.stdout')).open('w') as out, (r/(label+'-'+name+'.stderr')).open('w') as err:
        try:
            code = subprocess.run(argv, cwd=tree, env=env, stdout=out, stderr=err, timeout=limit).returncode
        except subprocess.TimeoutExpired:
            code = 'timeout'
    row = dict(argv=argv, name=name, cwd=str(tree), LUA_PATH=env['LUA_PATH'], ASAN_OPTIONS=env.get('ASAN_OPTIONS'), exit_code=code, seconds=time.monotonic()-st, stdout=label+'-'+name+'.stdout', stderr=label+'-'+name+'.stderr')
    rows.append(row)
    (r/(label+'-results.json')).write_text(json.dumps(rows, indent=2)+'\n')
    print(json.dumps(row), flush=True)
    return code == 0

for name in ['t-m8-finalizer-state', 't-m8-close-finalizers']:
    source = tree/'tests'/(name+'.c')
    exe = r/(label+'-'+name)
    inputs.append(source)
    argv = ['clang' if v == 'asan' else 'cc', '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-I'+str(s), *flags, str(source), str(s/'libluajit.a'), '-Wl,-E', '-lm', '-ldl', '-pthread', '-o', str(exe)]
    if run(argv, name+'-compile'):
        inputs.append(exe)
        run([str(exe)], name)
for name in ['t-ffi-gc-finreg', 't-m8-finalizer-spawn-live']:
    source = tree/'tests'/(name+'.lua')
    inputs.append(source)
    for off in [True, False]:
        run([str(s/'luajit'), *(['-joff'] if off else []), str(source)], name+('-joff' if off else '-jit'))
(r/(label+'-identities.json')).write_text(json.dumps({str(p.relative_to(r)): {'sha256': hashlib.sha256(p.read_bytes()).hexdigest(), 'bytes': p.stat().st_size} for p in inputs}, indent=2)+'\n')
raise SystemExit(0 if all(row['exit_code']==0 for row in rows) else 1)
