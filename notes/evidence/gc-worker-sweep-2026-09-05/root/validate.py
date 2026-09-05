from pathlib import Path
import hashlib, itertools, json, os, re, resource, subprocess, sys, time
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
p = Path(__file__).resolve().parent
kind = sys.argv[1]
tree = p / kind
out = p / ('results-' + kind)
out.mkdir(exist_ok=True)
asan = kind == 'asan'
flags = json.loads((p / (kind + '-build.json')).read_text())['flags']
san = ['-fsanitize=address', '-fno-omit-frame-pointer'] if asan else []
cc = 'clang' if asan else 'cc'
env = os.environ.copy()
env['LUA_PATH'] = str(tree / 'src/?.lua') + ';' + str(tree / 'tests/lib/?.lua') + ';;'
env['RETENTION_JIT'] = '0'
env.pop('ASAN_OPTIONS', None)
if asan:
    env['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1'
rows, identities = [], {}

def identify(f):
    f = Path(f).resolve()
    b = f.read_bytes()
    identities[str(f)] = dict(sha256=hashlib.sha256(b).hexdigest(), bytes=len(b))
    (out / 'identities.json').write_text(json.dumps(identities, indent=2) + '\n')

def run(name, argv, test=True, cwd=None, timeout=60):
    cwd = cwd or tree
    started = time.monotonic()
    timed = False
    with (out / (name + '.stdout')).open('wb') as so, (out / (name + '.stderr')).open('wb') as se:
        try:
            code = subprocess.run(argv, cwd=cwd, env=env, stdout=so, stderr=se, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            code, timed = 124, True
    rows.append(dict(name=name, command=argv, test=test, cwd=str(cwd),
                     environment={k: env[k] for k in ['LUA_PATH', 'RETENTION_JIT', 'ASAN_OPTIONS'] if k in env},
                     timeout_seconds=timeout, exit=code, timed_out=timed,
                     seconds=time.monotonic()-started, stdout=name+'.stdout', stderr=name+'.stderr'))
    (out / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
    print(kind, name, code, flush=True)
    return code

def ctest(name, source, modes, wraps=(), extra=(), timeout=60):
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
    for index, args in enumerate(modes):
        run(name + '-' + str(index),
            ['taskset', '-c', '0-15', str(exe), *args], timeout=timeout)

def verify():
    inputs = json.loads((p/'setup.json').read_text())['combined_inputs']
    for rel, expected in inputs.items():
        assert hashlib.sha256((tree/rel).read_bytes()).hexdigest() == expected, rel
    return inputs

(out/'source-identity.json').write_text(json.dumps(verify(), indent=2)+'\n')
identify(tree/'src/luajit')
identify(tree/'src/libluajit.a')
f = p/'fixtures'
for mode in ['-joff', '-jon']:
    run('stock'+mode, ['taskset','-c','0-15',str(tree/'src/luajit'),mode,'test.lua','--quiet'], cwd=tree/'tests/stock/test')
    script=tree/'tests/t-gc-generational-mode.lua'
    identify(script)
    run('generational'+mode, ['taskset','-c','0-15',str(tree/'src/luajit'),mode,str(script)])

hs = 'lj_safepoint_handshake'
scan = 'lj_gc2_scan_cycle_owner_tg_roots_native_parked'
prep = 'lj_arena_alloc_prepare_sweep_kind'
ctest('worker-stop', f/'t-worker-bridge-stop.c',
      [[str(a),str(w),str(l)] for a,w,l in itertools.product([0,1],[1,2],[0,1])], [hs], timeout=35)
ctest('worker-detach', f/'t-worker-bridge-detach.c', [['0'],['1']],
      [hs,'lj_gc2_flush_ssb_detach',scan,prep], timeout=35)
ctest('worker-consumed', f/'t-worker-bridge-consumed.c', [['0'],['1']],
      [hs,'lj_native_leave_tg','lj_tg_detach',scan,prep], timeout=35)
ctest('worker-quiet', f/'t-worker-bridge-quiet.c', [['1'],['2']],
      [hs,'lj_gc_sweep_gc2_unmarked'], timeout=35)
identify(f/'peer-control.lua')
ctest('automatic-retention', f/'t-string-retention.c',
      [['0',str(peer),str(workers),str(f/'peer-control.lua')] for peer,workers in itertools.product([0,1],[0,2])], timeout=50)

ctest('t-gc2-alloc-account', tree/'tests/t-gc2-alloc-account.c', [[]])
if kind != 'candidate':
    ctest('t-gc2-interp-hard-check', tree/'tests/t-gc2-interp-hard-check.c', [[]])
    ctest('t-func-construction-anchor', tree/'tests/t-func-construction-anchor.c', [[]], timeout=20)
    ctest('t-gc2-constructor-defer', tree/'tests/t-gc2-constructor-defer.c', [['publish'],['cancel'],['automatic']])
    ctest('t-gc2-constructor-mixed', tree/'tests/t-gc2-constructor-mixed.c', [['0','1'],['2','1']])
    ctest('t-gc2-constructor-fairness', tree/'tests/t-gc2-constructor-fairness.c',
          [['0','1','1'],['0','3','1'],['0','3','64'],['2','3','64'],['0','3','1','detach']])
    ctest('t-safepoint-native-root-hold', tree/'tests/t-safepoint-native-root-hold.c',
          [[str(n)] for n in range(4)], [scan,'lj_gc2_scan_cycle_global_roots','lj_gc2_flush_ssb'])
    ctest('t-safepoint-remote-root-completion', tree/'tests/t-safepoint-remote-root-completion.c',
          [[str(n)] for n in range(6)], [scan])
    for suffix in ['', '-forced']:
        ctest('t-safepoint-local-native-duplicate'+suffix,
              f/('t-safepoint-local-native-duplicate'+suffix+'.c'), [[]], [scan,'lj_lex_gc2_markroots'])
    for name in ['t-gc2-worker-scheduler', 't-gc2-recovery', 't-gc2-sweep-public-table-rescan',
                 't-gc2-sweep-leaf-publication', 't-gc2-sweep-edge-lease']:
        source = tree/'tests'/(name+'.c')
        wraps = sorted(set(re.findall(r'__wrap_([a-zA-Z0-9_]+)', source.read_text())))
        ctest(name, source, [[]], wraps)
    ctest('t-gc2-sweep-table-coalescing', tree/'tests/t-gc2-sweep-table-coalescing.c', [[]],
          ['calloc'], extra=['-DLJ_TEST_WRAP_CALLOC'])
verify()
summary = dict(runtime_pass=sum(r['test'] and r['exit']==0 for r in rows),
               runtime_fail=sum(r['test'] and r['exit']!=0 for r in rows),
               compilation_fail=sum(not r['test'] and r['exit']!=0 for r in rows),
               failures=[r['name'] for r in rows if r['exit']!=0])
(out/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
print(kind, json.dumps(summary), flush=True)
sys.exit(bool(summary['failures']))
