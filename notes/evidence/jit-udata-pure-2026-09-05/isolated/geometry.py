from pathlib import Path
import hashlib,json,os,subprocess,time
p=Path(__file__).resolve().parent;rows=[]
for trial in range(16):
 variant=['guarded','candidate'][trial%2];tree=p/variant;env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';;'
 cmd=['taskset','-c','31',str(tree/'src/luajit'),'-jon',str(p/'cost-geometry.lua'),'file','20000000',str(p/'hash-geometry.so')]
 start=time.monotonic();r=subprocess.run(cmd,cwd=tree,env=env,capture_output=True,text=True,timeout=30)
 rows.append({'trial':trial,'variant':variant,'command':cmd,'cwd':str(tree),'environment':{'LUA_PATH':env['LUA_PATH']},'seconds':time.monotonic()-start,'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr,'fixture_sha256':hashlib.sha256((p/'cost-geometry.lua').read_bytes()).hexdigest(),'module_sha256':hashlib.sha256((p/'hash-geometry.so').read_bytes()).hexdigest(),'exe_sha256':hashlib.sha256((tree/'src/luajit').read_bytes()).hexdigest()})
 (p/'geometry-results.json').write_text(json.dumps(rows,indent=2)+'\n')
 assert r.returncode==0,rows[-1]
 print(trial,variant,[l for l in r.stdout.splitlines() if l.startswith(('geometry','result'))],flush=True)
