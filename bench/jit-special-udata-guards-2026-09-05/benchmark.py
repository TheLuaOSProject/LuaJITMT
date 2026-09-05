from pathlib import Path
import hashlib,json,math,os,statistics,subprocess,time
p=Path(__file__).resolve().parent
rows=[]
for workload in ['clib','file','call','ffi_struct']:
 for pair in range(7):
  for variant in (['baseline','candidate'] if pair%2==0 else ['candidate','baseline']):
   tree=p/variant;env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';;';env['BENCH_SCALE']='1'
   if workload=='ffi_struct':fixture=tree/'aux/bench/bench.lua';args=[str(fixture),'ffi_struct']
   else:fixture=p/'cost.lua';args=[str(fixture),workload]
   cmd=['taskset','-c','31',str(tree/'src/luajit'),'-jon']+args
   start=time.monotonic();r=subprocess.run(cmd,cwd=tree,env=env,capture_output=True,text=True,timeout=45)
   row=dict(workload=workload,pair=pair,variant=variant,command=cmd,cwd=str(tree),environment={k:env[k] for k in ['LUA_PATH','BENCH_SCALE']},seconds=time.monotonic()-start,exit=r.returncode,stdout=r.stdout,stderr=r.stderr,fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(),exe_sha256=hashlib.sha256((tree/'src/luajit').read_bytes()).hexdigest())
   rows.append(row);(p/'cost-results.json').write_text(json.dumps(rows,indent=2)+'\n')
   assert r.returncode==0,row
  print(workload,'pair',pair,'complete',flush=True)
