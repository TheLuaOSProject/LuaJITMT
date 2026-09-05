from pathlib import Path
import subprocess,os,time,json,hashlib,concurrent.futures
r=Path(__file__).resolve().parent;old=Path('/tmp/lj-gc-auto-admission-20260905-h7ntx71p')
flags=['-DLUA_USE_ASSERT','-DLUA_USE_APICHECK','-DLJ_GC2_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_FUNC_TEST_HELPERS','-DLJ_UDATA_TEST_HELPERS','-DLJ_STR_TEST_HELPERS']
def variant(v,s):
 rows=[];ids={};env=dict(os.environ,LUA_PATH=str(s/'?.lua')+';'+str(s.parent/'tests/lib/?.lua')+';;')
 def run(argv,n):
  st=time.monotonic()
  with (r/(n+'.stdout')).open('w') as out,(r/(n+'.stderr')).open('w') as err:
   try:x=subprocess.run(argv,cwd=s.parent,env=env,stdout=out,stderr=err,timeout=60);code=x.returncode
   except subprocess.TimeoutExpired:code='timeout'
  rows.append(dict(name=n,argv=argv,exit_code=code,seconds=time.monotonic()-st,cwd=str(s.parent),LUA_PATH=env['LUA_PATH'],stdout=n+'.stdout',stderr=n+'.stderr'))
  (r/(v+'-matched-safety-results.json')).write_text(json.dumps(rows,indent=2)+'\n')
  print(n,code,flush=True)
  return code
 for f,wraps in [('t-gc2-worker-scheduler',['pthread_create','pthread_join']),('t-func-construction-anchor',[])]:
  e=r/(v+'-matched-'+f);fixture=r/'extrahelpers/tests'/(f+'.c')
  cmd=['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror',*flags,'-I'+str(s),str(fixture),str(s/'libluajit.a'),'-lm','-ldl','-pthread','-Wl,-E',*['-Wl,--wrap='+x for x in wraps],'-o',str(e)]
  if run(cmd,v+'-matched-'+f+'-compile'):continue
  for k in range(3 if wraps else 1):run([str(e)],v+'-matched-'+f+'-'+str(k))
  for p in [e,fixture,s/'libluajit.a']:ids[str(p)]=dict(sha256=hashlib.sha256(p.read_bytes()).hexdigest(),bytes=p.stat().st_size)
 (r/(v+'-matched-safety-identities.json')).write_text(json.dumps(ids,indent=2)+'\n')
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as ex:
 futures=[ex.submit(variant,v,s) for v,s in [('control',r/'controlextrahelpers/src'),('oldcandidate',old/'extrahelpers/src'),('veto',r/'extrahelpers/src')]]
 for f in futures:f.result()
