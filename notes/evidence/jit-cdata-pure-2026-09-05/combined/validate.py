from pathlib import Path
import hashlib
import json
import os
import resource
import subprocess
import sys
import time

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root = Path(__file__).resolve().parent
kind = sys.argv[1]
tree = root / kind
strict = kind != 'normal'
asan = kind == 'asan'
cc = 'clang' if asan else 'cc'
flags = (['-DLUA_USE_ASSERT', '-DLJ_XSAVE_TEST_HELPERS',
          '-DLJ_GC2_TEST_HELPERS', '-DLJ_TRACE_TEST_HELPERS',
          '-DLJ_TAB_TEST_HELPERS'] if strict else [])
sanflags = ['-fsanitize=address', '-fno-omit-frame-pointer'] if asan else []
env = os.environ.copy()
env['LUA_PATH'] = (str(tree/'tests/lib/?.lua')+';'+str(tree/'src/?.lua')+';'+
                   str(tree/'src/?/init.lua')+';;')
if asan:
    env['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1'
else:
    env.pop('ASAN_OPTIONS', None)
records = []
binaries = {}

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def run(name, command, cwd=tree, bound=30, test=False):
    start = time.monotonic()
    try:
        proc = subprocess.run(command, cwd=cwd, env=env, capture_output=True,
                              text=True, timeout=bound)
        result = dict(exit=proc.returncode, stdout=proc.stdout, stderr=proc.stderr)
    except subprocess.TimeoutExpired as exc:
        result = dict(exit=None, timeout=True,
                      stdout=(exc.stdout or b'').decode(errors='replace'),
                      stderr=(exc.stderr or b'').decode(errors='replace'))
    records.append(dict(name=name, command=command, cwd=str(cwd), test=test,
                        environment={k: v for k, v in env.items()
                                     if k in ('LUA_PATH', 'ASAN_OPTIONS', 'LJ_M7_FFI_CALLXS_SO')},
                        seconds=time.monotonic()-start, **result))
    (root/(kind+'-results.json')).write_text(json.dumps(records, indent=2)+'\n')
    print(kind, name, 'exit', result['exit'], flush=True)
    assert result['exit'] == 0, result['stderr']
    return result

command = ['taskset', '-c', '0-15', 'make', '-C', str(tree/'src'), '-j4']
if strict:
    command += ['BUILDMODE=static', 'CCDEBUG=-g', 'TARGET_STRIP=:',
                'XCFLAGS='+' '.join(flags)]
if asan:
    command += ['CC=clang', 'CCOPT=-O1',
                'TARGET_CFLAGS='+' '.join(sanflags),
                'TARGET_LDFLAGS=-fsanitize=address']
run('build', command, bound=240)
binaries.update({name: sha(tree/'src'/name) for name in ('luajit', 'libluajit.a')})
if asan:
    hostnm = run('host-sanitizer-symbol-check',
                 ['nm', str(tree/'src/host/minilua'), str(tree/'src/host/buildvm')])
    assert '__asan_' not in hostnm['stdout']
    targetnm = run('runtime-sanitizer-symbol-check', ['nm', str(tree/'src/lj_opt_loop.o')])
    assert '__asan_' in targetnm['stdout']

for mode in ('-joff', '-jon'):
    run('stock'+mode, ['taskset', '-c', '0-15', str(tree/'src/luajit'),
                      mode, 'test.lua', '--quiet'], tree/'tests/stock/test', test=True)
    for name in ('t-jit-cdata-basemt-guards.lua', 't-meta-cdata-capture.lua'):
        run(name+mode, ['taskset', '-c', '0-15', str(tree/'src/luajit'),
                       mode, str(tree/'tests'/name)], test=True)

cases = [('mutation-'+mode, 't-jit-cdata-pure.lua', [mode])
         for mode in ('index', 'newindex', 'missing', 'nonfunction', 'resize', 'methodlife', 'replace')]
cases.append(('side', 't-jit-cdata-pure-side.lua', []))
cases += [('exclude-'+mode, 't-jit-cdata-pure-exclusions.lua', [mode])
          for mode in ('allocate', 'luastore', 'newref', 'clear', 'foreign', 'indirect', 'fpmath')]
cases.append(('profile', 't-jit-cdata-pure-profile.lua', []))
for name, source, args in cases:
    run(name, ['taskset', '-c', '0-15', str(tree/'src/luajit'), '-jon',
               str(tree/'tests'/source)] + args, test=True)

fixtures = [('t-jit-cdata-pure-phase.c', [['gate'], ['worker']], []),
            ('t-jit-cdata-pure-error.c', [[]], ['-Wl,--wrap=lj_mem_realloc']),
            ('t-jit-first-attach.c', [[], ['noloop']], []),
            ('t-ffi-callxs-callback-stack.c', [[]], [])]
if strict:
    fixtures += [('t-jit-xsave.c', [[]], []),
                 ('t-ffi-callxs-callback.c', [[]], []),
                 ('t-ffi-callxs-postcall.c', [[]], [])]
    shared = root/(kind+'-authentic.so')
    run('authentic-shared-build', [cc, '-O2', '-shared', '-fPIC',
                                  str(tree/'tests/t-ffi-callxs-authentic-lib.c'), '-o', str(shared)])
    env['LJ_M7_FFI_CALLXS_SO'] = str(shared)
for name, argsets, extra in fixtures:
    binary = root/(kind+'-'+name[:-2])
    command = [cc, '-std=gnu11', '-O1' if asan else '-O2', '-g',
               '-Wall', '-Wextra', '-Werror', '-mcx16'] + flags + sanflags
    command += ['-I'+str(tree/'src'), '-I'+str(tree/'tests'),
                str(tree/'tests'/name), str(tree/'src/libluajit.a'),
                '-lm', '-ldl', '-pthread'] + extra + ['-o', str(binary)]
    run('compile-'+name, command)
    binaries[binary.name] = sha(binary)
    for args in argsets:
        run(name+' '+' '.join(args), ['taskset', '-c', '0-15', str(binary)]+args, test=True)
(root/(kind+'-binaries.json')).write_text(json.dumps(binaries, indent=2)+'\n')
