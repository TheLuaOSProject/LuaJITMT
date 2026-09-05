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
tree = root/kind
strict = kind != 'normal'
asan = kind == 'asan'
cc = 'clang' if asan else 'cc'
flags = (['-DLUA_USE_ASSERT', '-DLJ_FUNC_TEST_HELPERS', '-DLJ_GC2_TEST_HELPERS',
          '-DLJ_TAB_TEST_HELPERS', '-DLJ_ARENA_TEST_HELPERS',
          '-DLJ_TRACE_TEST_HELPERS', '-DLJ_XSAVE_TEST_HELPERS'] if strict else [])
san = ['-fsanitize=address', '-fno-omit-frame-pointer'] if asan else []
env = os.environ.copy()
env['LUA_PATH'] = str(tree/'tests/lib/?.lua')+';'+str(tree/'src/?.lua')+';;'
if asan:
    env['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1'
else:
    env.pop('ASAN_OPTIONS', None)
rows = []
binaries = {}
def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()
def run(name, cmd, cwd=tree, bound=30, test=False):
    start = time.monotonic()
    try:
        p = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True,
                           text=True, timeout=bound)
        result = dict(exit=p.returncode, stdout=p.stdout, stderr=p.stderr)
    except subprocess.TimeoutExpired as exc:
        result = dict(exit=None, timeout=True,
                      stdout=(exc.stdout or b'').decode(errors='replace'),
                      stderr=(exc.stderr or b'').decode(errors='replace'))
    rows.append(dict(name=name, command=cmd, cwd=str(cwd), test=test,
                     seconds=time.monotonic()-start, environment={
                         k:v for k,v in env.items() if k in ['LUA_PATH', 'ASAN_OPTIONS',
                         'LJ_LOADLIB_STOPREQ_SO', 'LJ_M7_FFI_CALLXS_FLUSH_SO']}, **result))
    (root/(kind+'-results.json')).write_text(json.dumps(rows, indent=2)+'\n')
    print(kind, name, 'exit', result['exit'], flush=True)
    assert result['exit'] == 0, result['stderr']
    return result

cmd = ['taskset', '-c', '0-15', 'make', '-C', str(tree/'src'), '-j4']
if strict:
    cmd += ['BUILDMODE=static', 'CCDEBUG=-g', 'TARGET_STRIP=:', 'XCFLAGS='+' '.join(flags)]
if asan:
    cmd += ['CC=clang', 'CCOPT=-O1', 'TARGET_CFLAGS='+' '.join(san),
            'TARGET_LDFLAGS=-fsanitize=address']
run('build', cmd, bound=240)
binaries.update({name: sha(tree/'src'/name) for name in ['luajit', 'libluajit.a']})
if asan:
    h = run('uninstrumented-generators', ['nm', str(tree/'src/host/minilua'), str(tree/'src/host/buildvm')])
    assert '__asan_' not in h['stdout']
    t = run('instrumented-runtime', ['nm', str(tree/'src/lj_safepoint.o')])
    assert '__asan_' in t['stdout']
for mode in ['-joff', '-jon']:
    run('stock'+mode, ['taskset', '-c', '0-15', str(tree/'src/luajit'), mode,
                      'test.lua', '--quiet'], tree/'tests/stock/test', test=True)

fixtures = [('t-ffi-callxs-callback-stack.c', [[]], [])]
if strict:
    loadlib = root/(kind+'-loadlib.so')
    run('compile-loadlib', [cc, '-O2', '-shared', '-fPIC', '-I'+str(tree/'src'),
                           str(tree/'tests/t-loadlib-stopreq-lib.c'), '-o', str(loadlib)])
    env['LJ_LOADLIB_STOPREQ_SO'] = str(loadlib)
    fixtures += [
        ('t-safepoint-remote-root-completion.c', [[str(i)] for i in range(6)],
         ['-Wl,--wrap=lj_gc2_scan_cycle_owner_tg_roots_native_parked']),
        ('t-safepoint-local-native-duplicate.c', [[]],
         ['-Wl,--wrap=lj_gc2_scan_cycle_owner_tg_roots_native_parked', '-Wl,--wrap=lj_lex_gc2_markroots']),
        ('t-safepoint-native-root-hold.c', [[str(i)] for i in range(4)],
         ['-Wl,--wrap=lj_gc2_scan_cycle_owner_tg_roots_native_parked',
          '-Wl,--wrap=lj_gc2_scan_cycle_global_roots', '-Wl,--wrap=lj_gc2_flush_ssb']),
        ('t-safepoint-handshake.c', [[]], ['-rdynamic'])]
for name, argsets, extra in fixtures:
    exe = root/(kind+'-'+name[:-2])
    cmd = [cc, '-std=gnu11', '-O1' if asan else '-O2', '-g', '-Wall', '-Wextra',
           '-Werror', '-mcx16']+flags+san+['-I'+str(tree/'src'), '-I'+str(tree/'tests'),
           str(tree/'tests'/name), str(tree/'src/libluajit.a'), '-lm', '-ldl', '-pthread']+extra+['-o', str(exe)]
    run('compile-'+name, cmd)
    binaries[exe.name] = sha(exe)
    for args in argsets:
        run(name+' '+' '.join(args), ['taskset', '-c', '0-15', str(exe)]+args, test=True)

remote = Path('/tmp/lj-remote-flush-readiness-20260905-root-i9ll10l3')
env['LJ_M7_FFI_CALLXS_FLUSH_SO'] = str(remote/'native-witness.so')
run('generated-remote-flush', ['taskset', '-c', '0-15', str(tree/'src/luajit'), '-jon',
                              str(remote/'final.lua')], test=True)
(root/(kind+'-binaries.json')).write_text(json.dumps(binaries, indent=2)+'\n')
