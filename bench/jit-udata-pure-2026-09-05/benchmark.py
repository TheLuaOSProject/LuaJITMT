from pathlib import Path
import hashlib,json,math,os,statistics,subprocess,time
p=Path(__file__).resolve().parent;rows=[]
for workload in ['direct_clib','direct_file','direct_buffer','direct_plain','clib','file','call','ffi_struct']:
 for pair in range(7):
  for variant in (['guarded','candidate'] if pair%2==0 else ['candidate','guarded']):
   tree=p/variant;env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';;';env['BENCH_SCALE']='1'
   if workload=='ffi_struct':fixture=tree/'aux/bench/bench.lua';args=[str(fixture),'ffi_struct']
   elif workload.startswith('direct_'):fixture=p/'direct-cost.lua';args=[str(fixture),workload[len('direct_'):]]
   else:fixture=p/'cost.lua';args=[str(fixture),workload]
   cmd=['taskset','-c','31',str(tree/'src/luajit'),'-jon']+args
   start=time.monotonic();r=subprocess.run(cmd,cwd=tree,env=env,capture_output=True,text=True,timeout=45)
   rows.append(dict(workload=workload,pair=pair,variant=variant,command=cmd,cwd=str(tree),environment={k:env[k] for k in ['LUA_PATH','BENCH_SCALE']},seconds=time.monotonic()-start,exit=r.returncode,stdout=r.stdout,stderr=r.stderr,fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(),exe_sha256=hashlib.sha256((tree/'src/luajit').read_bytes()).hexdigest()))
   (p/'cost-results.json').write_text(json.dumps(rows,indent=2)+'\n')
   assert r.returncode==0,rows[-1]
  print(workload,'pair',pair,'complete',flush=True)
