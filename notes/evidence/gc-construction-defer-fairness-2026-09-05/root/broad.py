from pathlib import Path
import hashlib, json, os, resource, subprocess, sys, time
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
p = Path(__file__).resolve().parent
kind = sys.argv[1]
tree = p / kind
out = p / ('broad-' + kind)
out.mkdir(exist_ok=True)
asan = kind == 'asan'
flags = json.loads((p / (kind + '-build.json')).read_text())['flags']
san = ['-fsanitize=address', '-fno-omit-frame-pointer'] if asan else []
cc = 'clang' if asan else 'cc'
env = os.environ.copy()
env['LUA_PATH'] = str(tree / 'src/?.lua') + ';' + str(tree / 'tests/lib/?.lua') + ';;'
if asan:
    env['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1'
else:
    env.pop('ASAN_OPTIONS', None)
rows, identities = [], {}

def identify(f):
    f = Path(f).resolve()
    b = f.read_bytes()
    identities[str(f)] = dict(sha256=hashlib.sha256(b).hexdigest(), bytes=len(b))
    (out / 'identities.json').write_text(json.dumps(identities, indent=2) + '\n')

def run(name, argv, test=True, cwd=tree):
    started = time.monotonic()
    timed = False
    with (out / (name + '.stdout')).open('wb') as so, (out / (name + '.stderr')).open('wb') as se:
        try:
            code = subprocess.run(argv, cwd=cwd, env=env, stdout=so, stderr=se, timeout=60).returncode
        except subprocess.TimeoutExpired:
            code, timed = 124, True
    row = dict(name=name, command=argv, test=test, cwd=str(cwd),
               environment={k: env[k] for k in ['LUA_PATH', 'ASAN_OPTIONS'] if k in env},
               timeout_seconds=60, exit=code, timed_out=timed,
               seconds=time.monotonic()-started, stdout=name+'.stdout', stderr=name+'.stderr')
    rows.append(row)
    (out / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
    print(kind, name, code, flush=True)
    return code

def ctest(name, source, modes, wraps=(), extra=()):
    source = Path(source)
    identify(source)
    exe, dep = out / name, out / (name + '.d')
    cmd = [cc, '-std=gnu11', '-O1' if asan else '-O2', '-g', '-Wall', '-Wextra',
           '-Werror', '-mcx16', *flags, *san, *extra, '-MMD', '-MF', str(dep),
           '-I'+str(tree/'src'), '-I'+str(tree/'tests'), str(source),
           str(tree/'src/libluajit.a'), '-Wl,-E', '-lm', '-ldl', '-pthread',
           *['-Wl,--wrap='+w for w in wraps], '-o', str(exe)]
    if run(name+'-compile', cmd, test=False):
        return
    identify(exe)
    for token in dep.read_text().split(':', 1)[1].replace('\\\n', ' ').split():
        identify(token)
    for args in modes:
        run(name + ('-' + '-'.join(args) if args else ''), ['taskset', '-c', '0-15', str(exe), *args])

identify(tree/'src/luajit')
identify(tree/'src/libluajit.a')
setup = json.loads((p/'setup.json').read_text())
actual = {}
for rel, expected in setup['combined_inputs'].items():
    digest = hashlib.sha256((tree/rel).read_bytes()).hexdigest()
    assert digest == expected, rel
    actual[rel] = digest
(out/'source-identity.json').write_text(json.dumps(actual, indent=2)+'\n')

assert kind in ['strict', 'asan']
selection = json.loads((p/'broad-selection.json').read_text())
for spec in selection['selected']:
    extra = ['-DLJ_TEST_WRAP_CALLOC'] if spec['name']=='t-gc2-sweep-table-coalescing' else []
    ctest(spec['name'], tree/spec['source'], spec['modes'], spec['wraps'], extra)
for name in ['t-gc-active-thread-roots', 't-gc-workers', 't-gc2-finalizer-peer-collect']:
    f = tree/'tests'/(name+'.lua')
    identify(f)
    for mode in ['-joff', '-jon']:
        run(name+mode, ['taskset','-c','0-15',str(tree/'src/luajit'),mode,str(f)])

summary = dict(runtime_pass=sum(r['test'] and r['exit']==0 for r in rows),
               runtime_fail=sum(r['test'] and r['exit']!=0 for r in rows),
               compilation_fail=sum(not r['test'] and r['exit']!=0 for r in rows),
               failures=[r['name'] for r in rows if r['exit']!=0])
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(kind, json.dumps(summary), flush=True)
sys.exit(bool(summary['failures']))
