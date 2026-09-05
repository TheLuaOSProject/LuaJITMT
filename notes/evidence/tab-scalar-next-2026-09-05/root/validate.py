from pathlib import Path
import hashlib, itertools, json, os, resource, subprocess, sys, time

resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent
kind=sys.argv[1]
assert kind in ['candidate','optimized','strict','asan','baseline']
base=kind=='baseline'
tree=Path('/tmp/lj-worker-bridge-combined-20260905-bz9wysjp/strict') if base else p/kind
config='strict' if base else kind
build=json.loads((p/(config+'-build.json')).read_text())
flags=build['flags']
asan=kind=='asan'
out=p/('results-'+kind)
out.mkdir(exist_ok=False)
setup=json.loads((p/'setup.json').read_text())
inputs=setup['before_inputs' if base else 'combined_inputs']
sha=lambda f:hashlib.sha256(Path(f).read_bytes()).hexdigest()
env=os.environ.copy()
env.update(LUA_PATH=str(tree/'src/?.lua')+';'+str(tree/'tests/lib/?.lua')+';;')
env.pop('ASAN_OPTIONS',None)
if asan:
    env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
cpus={'candidate':'0-3','optimized':'4-7','strict':'8-11','asan':'12-15','baseline':'28-29'}[kind]
rows=[]
identities={}

def verify():
    for rel,digest in inputs.items():
        assert sha(tree/rel)==digest,rel

def identify(f):
    f=Path(f).resolve()
    identities[str(f)]=dict(sha256=sha(f),bytes=f.stat().st_size)

def run(name,cmd,test=True,expected=0,cwd=None,timeout=20):
    start=time.monotonic()
    row=dict(name=name,command=cmd,cwd=str(cwd or tree),test=test,expected_exit=expected,
        environment={k:env[k] for k in ['LUA_PATH','ASAN_OPTIONS'] if k in env},timeout_seconds=timeout)
    with (out/(name+'.stdout')).open('wb') as so,(out/(name+'.stderr')).open('wb') as se:
        try:
            row['exit']=subprocess.run(cmd,cwd=cwd or tree,env=env,stdout=so,stderr=se,timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            row['exit'],row['timed_out']=124,True
    row['seconds']=time.monotonic()-start
    rows.append(row)
    (out/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
    print(kind,name,row['exit'],flush=True)
    assert row['exit']==expected,(name,row['exit'],expected)

def ctest(name,source,modes,expected=0,timeout=20):
    src=p/'fixtures'/source
    exe=out/name
    dep=out/(name+'.d')
    identify(src)
    cmd=['clang' if asan else 'cc','-std=gnu11','-O1' if asan else '-O2','-g',
        '-Wall','-Wextra','-Werror','-mcx16',*flags]
    if asan:cmd+=['-fsanitize=address','-fno-omit-frame-pointer']
    cmd+=['-MMD','-MF',str(dep),'-I'+str(tree/'src'),'-I'+str(tree/'tests'),
        '-I'+str(p/'fixtures'),str(src),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)]
    run(name+'-compile',cmd,test=False,timeout=40)
    identify(exe)
    for rel in dep.read_text().replace('\\\n',' ').split(':',1)[1].split():
        identify(rel)
    (out/'identities.json').write_text(json.dumps(identities,indent=2)+'\n')
    for index,args in enumerate(modes):
        run(name+'-'+str(index),['taskset','-c',cpus,str(exe),*args],expected=expected,timeout=timeout)

verify()
(out/'source-before.json').write_text(json.dumps(inputs,indent=2)+'\n')
identify(tree/'src/luajit')
identify(tree/'src/libluajit.a')
if base:
    ctest('progress','t-tab-scalar-next-progress.c',[['next','dense'],['rooted','empty']],expected=-14)
else:
    for mode in ['-joff','-jon']:
        run('stock'+mode,['taskset','-c',cpus,str(tree/'src/luajit'),mode,'test.lua','--quiet'],
            cwd=tree/'tests/stock/test',timeout=60)
    if kind=='candidate':
        for mode in ['-joff','-jon']:
            f=tree/'tests/t-gc-generational-mode.lua'
            identify(f)
            run('generational'+mode,['taskset','-c',cpus,str(tree/'src/luajit'),mode,str(f)])
    else:
        ctest('idle','t-jit-idle-reclaim-entry.c',[[]])
        ctest('progress','t-tab-scalar-next-progress.c',
              [list(x) for x in itertools.product(['next','itern','rooted','cursor'],
                                                 ['dense','sparse','empty','holes','zero','bool'])])
        if kind in ['strict','asan']:
            ctest('authority','t-tab-scalar-next-authority-v6.c',[[x] for x in
                ['basic-colo','basic-separate','keys','aliases','opaque','protected','bounds',
                 'hooks-found','hooks-end','resize','plain']])
            ctest('stack-retry','t-tab-scalar-next-stack-retry.c',
                  [[mode,*slots] for mode in ['found','end'] for slots in [['2','3'],['1','3'],['0','1']]])
            ctest('lifetime','t-tab-scalar-next-lifetime.c',[['preflight'],['retired']])
            ctest('scalar-hit','t-tab-scalar-hit.c',[[]])
            ctest('rooted-reader','t-tab-rooted-reader.c',[[]])
verify()
(out/'identities.json').write_text(json.dumps(identities,indent=2)+'\n')
(out/'final.json').write_text(json.dumps(dict(runtime_inputs_verified=len(inputs),
    runtime_processes=sum(x['test'] for x in rows),positive_runtime_processes=sum(x['test'] and x['exit']==0 for x in rows),
    expected_failures=sum(x['test'] and x['expected_exit']!=0 for x in rows),
    all_expected=True),indent=2)+'\n')
