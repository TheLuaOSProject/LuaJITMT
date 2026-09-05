from pathlib import Path
import hashlib,json,os,resource,subprocess,sys,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent
variant=sys.argv[1]; assert variant in ['candidate','optimized','asan']
tree=p/variant; out=p/('stock-'+variant+'-results.json'); assert not out.exists()
sha=lambda f:hashlib.sha256(Path(f).read_bytes()).hexdigest()
inputs={str(f):sha(f) for f in [p/'run-stock.py',p/'broad-inputs.json',tree/'src/luajit',tree/'src/libluajit.a',tree/'src/jit/vmdef.lua']}
env={'LUA_PATH':str(tree/'src/?.lua')+';;'}
if variant=='asan': env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
rows=[]
for mode in ['-joff','-jon']:
    cmd=['taskset','-c','24-25',str(tree/'src/luajit'),mode,'test.lua']
    start=time.monotonic()
    try:
        q=subprocess.run(cmd,cwd=p/'fixtures/stock/test',env={**os.environ,**env},capture_output=True,text=True,timeout=45)
        result={'exit':q.returncode,'stdout':q.stdout,'stderr':q.stderr}
    except subprocess.TimeoutExpired as e:
        dec=lambda v:v.decode(errors='replace') if isinstance(v,bytes) else v or ''
        result={'exit':None,'timeout':True,'stdout':dec(e.stdout),'stderr':dec(e.stderr)}
    rows.append({'command':cmd,'cwd':str(p/'fixtures/stock/test'),'inputs':inputs,'environment':env,'timeout_seconds':45,'seconds':time.monotonic()-start,**result})
    out.write_text(json.dumps(rows,indent=2)+'\n')
    print(variant,mode,'stock exit',result['exit'],flush=True)
