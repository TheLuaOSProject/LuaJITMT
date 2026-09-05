from pathlib import Path
import hashlib,json,os,resource,subprocess,sys,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent;kind=sys.argv[1];tree=p/kind
fixture=p/'t-jit-special-udata-guards.lua'
env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';'+str(tree/'tests/lib/?.lua')+';;'
if kind=='asan':env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
else:env.pop('ASAN_OPTIONS',None)
rows=[]
for typ in ['clib','file','buffer','plain']:
 for mode in ['function','table','missing','nonfunction','replace','replace_missing','resize','methodlife','table_entry','newindex','newindex_table']:
  for jitmode in ['-joff','-jon']:
   cmd=['taskset','-c','0-15',str(tree/'src/luajit'),jitmode,str(fixture),typ,mode];start=time.monotonic()
   try:
    r=subprocess.run(cmd,cwd=tree,env=env,capture_output=True,text=True,timeout=20)
    result=dict(exit=r.returncode,stdout=r.stdout,stderr=r.stderr)
   except subprocess.TimeoutExpired as e:
    result=dict(exit=None,timeout=True,stdout=(e.stdout or b'').decode(errors='replace'),stderr=(e.stderr or b'').decode(errors='replace'))
   row=dict(kind=kind,udtype=typ,mode=mode,jitmode=jitmode,command=cmd,cwd=str(tree),seconds=time.monotonic()-start,environment={k:env[k] for k in ['LUA_PATH','ASAN_OPTIONS'] if k in env},fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(),exe_sha256=hashlib.sha256((tree/'src/luajit').read_bytes()).hexdigest(),**result)
   rows.append(row);(p/(kind+'-fixture-results.json')).write_text(json.dumps(rows,indent=2)+'\n')
   if result['exit']!=0:print(kind,typ,mode,jitmode,'FAIL',result['exit'],result['stderr'].splitlines()[:1],flush=True)
print(kind,'processes',len(rows),'pass',sum(r['exit']==0 for r in rows),'fail',sum(r['exit']!=0 for r in rows),flush=True)
