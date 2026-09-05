from pathlib import Path
import hashlib,itertools,json,os,resource,subprocess,sys,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent
kind=sys.argv[1]
setup=json.loads((p/'setup.json').read_text())
variant=setup['variants'][kind]
tree=Path(variant['tree'])
out=p/('results-v2-'+kind)
out.mkdir(exist_ok=False)
sha=lambda f:hashlib.sha256(Path(f).read_bytes()).hexdigest()
def verify():
    for f,d in setup['runtime_inputs'].items():assert sha(tree/f)==d,(kind,f)
verify()
asan=kind=='asan'
flags=variant['build']['flags']
cmd=['clang' if asan else 'cc','-std=gnu11','-O1' if asan else '-O2','-g',
     '-Wall','-Wextra','-Werror','-mcx16',*flags]
if asan:cmd+=['-fsanitize=address','-fno-omit-frame-pointer']
src=p/'fixtures/t-gc2-workclass-fairness-v2.c'
cmd+=['-MMD','-MF',str(out/'deps.d'),'-I'+str(tree/'src'),'-I'+str(tree/'tests'),
      str(src),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(out/'fixture')]
q=subprocess.run(cmd,cwd=p,capture_output=True,text=True,timeout=60)
(out/'compile.json').write_text(json.dumps(dict(command=cmd,cwd=str(p),exit=q.returncode,
    stdout=q.stdout,stderr=q.stderr),indent=2)+'\n')
assert q.returncode==0,q.stderr
identities={f:dict(sha256=sha(f),bytes=Path(f).stat().st_size)
    for f in (out/'deps.d').read_text().replace('\\\n',' ').split(':',1)[1].split()}
for f in [out/'fixture',tree/'src/libluajit.a',p/'run-v2.py']:
    identities[str(f)]=dict(sha256=sha(f),bytes=f.stat().st_size)
(out/'identities.json').write_text(json.dumps(identities,indent=2)+'\n')
env=os.environ.copy();env.pop('ASAN_OPTIONS',None)
env['LUA_PATH']=str(tree/'src/?.lua')+';;'
if asan:env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
rows=[]
for phase,quota,feed in itertools.product(['mark','sweep'],['1','64'],['0','1']):
    cmd=['taskset','-c','0-15',str(out/'fixture'),phase,quota,feed]
    start=time.monotonic()
    try:
        q=subprocess.run(cmd,cwd=p,env=env,capture_output=True,text=True,timeout=25)
        fields=dict(exit=q.returncode,stdout=q.stdout,stderr=q.stderr)
    except subprocess.TimeoutExpired as e:
        fields=dict(exit=124,timed_out=True,stdout=(e.stdout or b'').decode(errors='replace'),stderr=(e.stderr or b'').decode(errors='replace'))
    row=dict(command=cmd,cwd=str(p),environment={k:env[k] for k in ['LUA_PATH','ASAN_OPTIONS'] if k in env},
        timeout_seconds=25,seconds=time.monotonic()-start,expected_exit=2 if feed=='1' else 0,**fields)
    rows.append(row);(out/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
    print(kind,phase,quota,feed,row['exit'],row['stdout'].strip(),flush=True)
    if row['exit'] not in [0,2]:break
verify()
