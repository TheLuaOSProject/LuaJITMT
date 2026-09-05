from pathlib import Path
import subprocess,os,json
r=Path('/tmp/lj-premt-cdata-hoist-20260905-oa96m15y');out=r/'eligibility';out.mkdir(exist_ok=True);results=[]
for v in ['base-normal','fix-normal','fix-assert']:
 for mode in ['allocate','luastore','newref','clear','foreign','callback','indirect','fpmath']:
  env=os.environ.copy();env['LUA_PATH']=str(r/v/'src/?.lua')+';;'
  cmd=['taskset','-c','0-15',str(r/v/'src/luajit'),str(r/'eligibility.lua'),mode]
  p=subprocess.run(cmd,capture_output=True,text=True,env=env,timeout=20)
  for k in ['stdout','stderr']:(out/(v+'-'+mode+'.'+k)).write_text(getattr(p,k))
  results.append(dict(variant=v,mode=mode,command=cmd,exit=p.returncode,stdout=p.stdout,stderr=p.stderr))
  print(v,mode,p.returncode,p.stderr[:150],flush=True)
(r/'eligibility-results.json').write_text(json.dumps(results,indent=2)+'\n')
