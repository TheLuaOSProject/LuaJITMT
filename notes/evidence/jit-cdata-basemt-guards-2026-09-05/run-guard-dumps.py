from pathlib import Path
import subprocess,json,os,time,hashlib
r=Path(__file__).resolve().parent;rows=[]
for variant in ('base-normal','fix-normal'):
 tree=r/variant
 for workload,args in {'ffi_struct':['aux/bench/bench.lua','ffi_struct'],'constructor_sink':[str(r/'constructor-cost.lua'),'sink','80'],'constructor_nosink':[str(r/'constructor-cost.lua'),'nosink','80'],'native_call':[str(r/'t-jit-cdata-basemt-guards.lua'),'call']}.items():
  name='dump-'+variant+'-'+workload;env=os.environ.copy();override={'LUA_PATH':str(tree/'src/?.lua')+';;','BENCH_SCALE':'0.00001'};env.update(override)
  cmd=['taskset','-c','0-15',str(tree/'src/luajit'),'-jon','-jdump=im']+args
  t=time.monotonic();p=subprocess.run(cmd,cwd=tree,env=env,capture_output=True,text=True,timeout=20)
  (r/(name+'.stdout')).write_text(p.stdout);(r/(name+'.stderr')).write_text(p.stderr)
  rows.append({'name':name,'command':cmd,'cwd':str(tree),'environment_overrides':override,'exit':p.returncode,'seconds':time.monotonic()-t,'runtime_sha256':hashlib.sha256((tree/'src/luajit').read_bytes()).hexdigest()})
  print(name,p.returncode,flush=True)
(r/'guard-dump-results.json').write_text(json.dumps(rows,indent=2)+'\n')
