from pathlib import Path
import subprocess,os,json,hashlib
r=Path('/tmp/lj-premt-cdata-hoist-20260905-oa96m15y');out=r/'semantic';out.mkdir(exist_ok=True)
results=[]
for variant in ['base-normal','fix-normal','fix-assert']:
 for mode in ['index','newindex','missing','nonfunction','resize','methodlife','replace']:
  cmd=['taskset','-c','0-15',str(r/variant/'src/luajit'),str(r/'t-premt-cdata-pure.lua'),mode]
  if variant=='base-normal':cmd+=['baseline']
  env=os.environ.copy();env['LUA_PATH']=str(r/variant/'src/?.lua')+';;'
  p=subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=20)
  stem=variant+'-'+mode
  for k in ['stdout','stderr']:(out/(stem+'.'+k)).write_text(getattr(p,k))
  results.append(dict(variant=variant,mode=mode,command=cmd,env={'LUA_PATH':env['LUA_PATH']},exit=p.returncode,stdout=p.stdout,stderr=p.stderr))
  print(variant,mode,p.returncode,flush=True)
(r/'semantic-results.json').write_text(json.dumps(results,indent=2)+'\n')
