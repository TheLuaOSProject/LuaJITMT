from pathlib import Path
import hashlib,json,os,subprocess,sys,time,resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0));p=Path(__file__).resolve().parent
rows=[];variant=sys.argv[1];tree=p/variant;env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';;'
cases={'function':['false','nil','other','fenv','close'],'read':['false','nil','other','close'],'write':['false','nil','other','close'],'zero':['negative-zero','positive-zero','false','nil'],'big':['number','false','nil']}
for kind,modes in cases.items():
 for mode in modes:
  for target in ['root','side']:
   for status in ['-joff','-jon']:
    cmd=[str(tree/'src/luajit'),status,str(p/'t-clib-cache-authority.lua'),kind,mode,target,str(p/'authority.so')]+(['helper'] if variant=='prototype' else [])
    start=time.monotonic()
    try:
     r=subprocess.run(cmd,cwd=tree,env=env,capture_output=True,text=True,timeout=30);res={'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
    except subprocess.TimeoutExpired as e:res={'exit':None,'timeout':True,'stdout':(e.stdout or b'').decode(errors='replace'),'stderr':(e.stderr or b'').decode(errors='replace')}
    rows.append({'kind':kind,'mode':mode,'target':target,'status':status,'command':cmd,'cwd':str(tree),'environment':{'LUA_PATH':env['LUA_PATH']},'seconds':time.monotonic()-start,'inputs':{f:hashlib.sha256((p/f).read_bytes()).hexdigest() for f in ['t-clib-cache-authority.lua','authority.so']},'binary_sha256':hashlib.sha256((tree/'src/luajit').read_bytes()).hexdigest(),**res})
    (p/(variant+'-authority-results.json')).write_text(json.dumps(rows,indent=2)+'\n')
    if res['exit']!=0:print(variant,kind,mode,target,status,res,flush=True)
 print(variant,kind,'complete',flush=True)
print(variant,'passes',sum(row['exit']==0 for row in rows),'of',len(rows),flush=True)
