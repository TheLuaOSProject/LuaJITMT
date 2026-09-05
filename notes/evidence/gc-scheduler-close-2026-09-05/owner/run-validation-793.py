from pathlib import Path
import json,subprocess,time,os,hashlib,resource
P=Path(__file__).resolve().parent;A=P/'validation-793-fixture-headers';fixture=P/'candidate/tests/t-gc2-worker-scheduler.c'
resource.setrlimit(resource.RLIMIT_CORE,(0,0));rows=[];ids={}
def identify(q):
 q=Path(q);ids[str(q)]=dict(sha256=hashlib.sha256(q.read_bytes()).hexdigest(),bytes=q.stat().st_size)
 (A/'identities.json').write_text(json.dumps(ids,indent=2)+'\n')
identify(fixture)
for variant in json.loads((A/'variants.json').read_text()):
 name=variant['name'];S=Path(variant['source']);flags=variant['flags'][:];asan=variant['asan'];exe=A/name
 if asan:flags+=['-fsanitize=address','-fno-omit-frame-pointer']
 env=os.environ.copy();env['LUA_PATH']=str(S/'src/?.lua')+';'+str(S/'tests/lib/?.lua')+';;'
 if asan:env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
 archive=S/'src/libluajit.a';identify(archive)
 for q in [S/'src/lj_gc.c',S/'src/lj_gc2.c',S/'src/lj_obj.h',S/'src/lj_thr.c',S/'src/lj_state.c',S/'src/lj_tg.c']:identify(q)
 compile=['clang' if asan else 'cc','-std=gnu11','-O1' if asan else '-O2','-g','-Wall','-Wextra','-Werror','-I'+str(S/'src'),'-I'+str(S/'tests'),'-I'+str(A/'include'),*flags,str(fixture),str(archive),'-Wl,-E','-lm','-ldl','-pthread','-Wl,--wrap=pthread_create','-Wl,--wrap=pthread_join','-o',str(exe)]
 for kind,argv in [('compile',compile),('runtime',[str(exe)])]:
  start=time.monotonic();timed=False;stem=name+'-'+kind
  with (A/(stem+'.stdout')).open('wb') as out,(A/(stem+'.stderr')).open('wb') as err:
   try:r=subprocess.run(argv,cwd=S,env=env,stdout=out,stderr=err,timeout=60);code=r.returncode
   except subprocess.TimeoutExpired:timed=True;code=124
  row=dict(name=name,kind=kind,argv=argv,cwd=str(S),LUA_PATH=env['LUA_PATH'],ASAN_OPTIONS=env.get('ASAN_OPTIONS'),timeout_seconds=60,exit_code=code,timed_out=timed,seconds=time.monotonic()-start,stdout=stem+'.stdout',stderr=stem+'.stderr');rows.append(row)
  (A/'results.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps({k:row[k] for k in ['name','kind','exit_code','seconds']}),flush=True)
  if kind=='compile':
   if code:break
   identify(exe)
