from pathlib import Path
import subprocess,os,json,shutil,time,hashlib,re,statistics
r=Path('/tmp/lj-premt-cdata-hoist-20260905-oa96m15y');w=r/'workloads';w.mkdir(exist_ok=True)
src=Path('/tmp/lj-ffi-owned-trace-20260905-pmxukjxp/base-normal/aux/bench/bench.lua');shutil.copy2(src,w/'bench.lua');rows=[]
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
for v in ['base-normal','fix-normal']:
 env=os.environ.copy();override={'LUA_PATH':str(r/v/'src/?.lua')+';;','BENCH_SCALE':'0.00001'};env.update(override)
 cmd=['taskset','-c','0-15',str(r/v/'src/luajit'),'-jon','-jdump=im',str(w/'bench.lua'),'ffi_struct']
 p=subprocess.run(cmd,cwd=r/v,env=env,capture_output=True,text=True,timeout=20)
 for k in ['stdout','stderr']:(r/(v+'-ffi-struct-dump.'+k)).write_text(getattr(p,k))
 rows.append(dict(kind='dump',variant=v,command=cmd,environment_overrides=override,exit=p.returncode,runtime_sha256=sha(r/v/'src/luajit')))
 assert p.returncode==0,p.stderr
samples=[];cost=r/'cost';cost.mkdir(exist_ok=True)
for pair in range(1,8):
 order=['base-normal','fix-normal'] if pair%2 else ['fix-normal','base-normal']
 for v in order:
  env=os.environ.copy();override={'LUA_PATH':str(r/v/'src/?.lua')+';;','BENCH_SCALE':'1'};env.update(override)
  cmd=['taskset','-c','31',str(r/v/'src/luajit'),'-jon',str(w/'bench.lua'),'ffi_struct'];start=time.monotonic()
  p=subprocess.run(cmd,cwd=r/v,env=env,capture_output=True,text=True,timeout=20)
  name=f'pair{pair}-{v}'
  for k in ['stdout','stderr']:(cost/(name+'.'+k)).write_text(getattr(p,k))
  match=re.search(r'ffi_struct\s+(\d+)\s+([\d.]+)\s+([\d.]+)',p.stdout)
  row=dict(pair=pair,variant=v,command=cmd,cwd=str(r/v),environment_overrides=override,exit=p.returncode,wall_seconds=time.monotonic()-start,stdout=p.stdout,stderr=p.stderr,runtime_sha256=sha(r/v/'src/luajit'))
  samples.append(row);print(pair,v,p.returncode,p.stdout.strip(),flush=True)
  assert p.returncode==0,p.stderr
(r/'field-cost-results.json').write_text(json.dumps({'workload_source':str(src),'workload_sha256':sha(w/'bench.lua'),'dumps':rows,'samples':samples},indent=2)+'\n')
