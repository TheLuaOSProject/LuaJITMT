from pathlib import Path
import hashlib,json,os,subprocess,time
p=Path(__file__).resolve().parent
base=Path('/tmp/lj-special-udata-method-review-20260905-djl20ksd')
res=[]
for variant,mode in [('baseline','-joff'),('baseline','-jon'),('candidate','-joff'),('candidate','-jon'),('strict','-jon'),('asan','-jon')]:
 for fixture_name, args in [('clib-cache-lifecycle.lua',['env']),('clib-cache-lifecycle.lua',['closed'])]:
  tree=base/variant;exe=tree/'src/luajit';fixture=p/fixture_name
  env=dict(os.environ,LUA_PATH=f'{tree}/src/?.lua;{tree}/tests/lib/?.lua;;',ASAN_OPTIONS='detect_leaks=1:abort_on_error=1')
  cmd=[str(exe),mode,str(fixture)]+args;st=time.monotonic()
  try:
   r=subprocess.run(cmd,cwd=tree,env=env,text=True,capture_output=True,timeout=20);rc=r.returncode;out=r.stdout;err=r.stderr
  except subprocess.TimeoutExpired as e:
   rc='timeout';out=e.stdout;err=e.stderr
  entry=dict(variant=variant,command=cmd,cwd=str(tree),environment={k:env[k] for k in ('LUA_PATH','ASAN_OPTIONS')},seconds=time.monotonic()-st,exit=rc,stdout=out,stderr=err,fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(),exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),source_sha256=hashlib.sha256((tree/'src/lj_record.c').read_bytes()).hexdigest())
  res.append(entry);(p/'additional-results.json').write_text(json.dumps(res,indent=2)+'\n')
  print(variant,mode,args,rc,out,err)
