from pathlib import Path
import hashlib, json, os, resource, subprocess, sys, time
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
p = Path(__file__).resolve().parent
setup = json.loads((p/'setup.json').read_text())
runtime = Path(setup['runtime_package'])
kind = sys.argv[1]
tree = runtime/kind
out = p/kind
out.mkdir(exist_ok=True)
sha = lambda f: hashlib.sha256(f.read_bytes()).hexdigest()
for rel, expected in setup['runtime_inputs'].items(): assert sha(tree/rel) == expected, rel
build = json.loads((runtime/(kind+'-build.json')).read_text())
flags = build['flags']
asan = kind == 'asan'
env = dict(os.environ)
env['LUA_PATH'] = str(tree/'src/?.lua')+';;'
env.pop('ASAN_OPTIONS',None)
if asan: env['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1'
rows, identities = [], {}
def identify(f):
    f=Path(f).resolve();identities[str(f)]={'sha256':sha(f),'bytes':f.stat().st_size}
    (out/'identities.json').write_text(json.dumps(identities,indent=2)+'\n')
def run(name,cmd,test):
    row=dict(name=name,command=cmd,test=test,timeout_seconds=45,
             environment={k:env[k] for k in ['LUA_PATH','ASAN_OPTIONS'] if k in env})
    started=time.monotonic()
    with (out/(name+'.stdout')).open('wb') as so,(out/(name+'.stderr')).open('wb') as se:
        try: row['exit']=subprocess.run(cmd,cwd=p,env=env,stdout=so,stderr=se,timeout=45).returncode
        except subprocess.TimeoutExpired: row['exit'],row['timed_out']=124,True
    row['seconds']=time.monotonic()-started
    rows.append(row);(out/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
    print(kind,name,row['exit'],flush=True)
    assert row['exit']==0,row
identify(tree/'src/libluajit.a')
for name in ['ordinary','forced']:
    source=p/(name+'.c');exe=out/name;dep=out/(name+'.d')
    identify(source)
    cmd=['clang' if asan else 'cc','-std=gnu11','-O1' if asan else '-O2','-g','-Wall','-Wextra','-Werror','-mcx16',*flags]
    if asan:cmd+=['-fsanitize=address','-fno-omit-frame-pointer']
    cmd+=['-MMD','-MF',str(dep),'-I'+str(tree/'src'),str(source),str(tree/'src/libluajit.a'),'-Wl,-E','-lm','-ldl','-pthread',
          '-Wl,--wrap=lj_gc2_scan_cycle_owner_tg_roots_native_parked','-Wl,--wrap=lj_lex_gc2_markroots','-o',str(exe)]
    run(name+'-compile',cmd,False);identify(exe)
    for item in dep.read_text().split(':',1)[1].replace('\\\n',' ').split():identify(item)
    run(name,['taskset','-c','0-15',str(exe)],True)
for rel,expected in setup['runtime_inputs'].items():assert sha(tree/rel)==expected,rel
(out/'summary.json').write_text(json.dumps(dict(runtime_pass=2,runtime_fail=0,runtime_inputs_verified=len(setup['runtime_inputs']),build_command=build['command'],flags=flags),indent=2)+'\n')
