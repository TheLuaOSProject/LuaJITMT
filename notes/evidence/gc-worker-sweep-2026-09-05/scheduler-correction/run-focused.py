from pathlib import Path
import json,subprocess,os,hashlib,time,resource
P=Path(__file__).resolve().parent;D=P/'focused';D.mkdir();S=json.loads((P/'validation-inputs.json').read_text());rows=[]
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
def run(name,kind,argv,source,env,identities=None):
 start=time.monotonic();timed=False
 with (D/(name+'.stdout')).open('wb') as out,(D/(name+'.stderr')).open('wb') as err:
  try:r=subprocess.run(argv,cwd=source,env=env,stdout=out,stderr=err,timeout=60);code=r.returncode
  except subprocess.TimeoutExpired:code=124;timed=True
 row=dict(name=name,kind=kind,argv=argv,cwd=str(source),environment={k:env.get(k) for k in ['LUA_PATH','ASAN_OPTIONS','LSAN_OPTIONS','LD_PRELOAD','LD_LIBRARY_PATH']},timeout_seconds=60,exit_code=code,timed_out=timed,seconds=time.monotonic()-start)
 if identities:row['identities']={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in identities if f.exists()}
 rows.append(row);(D/'results.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps({k:row[k] for k in ['name','kind','exit_code','timed_out','seconds']}),flush=True);return code
for inp in S:
 if inp['configuration']!='default':continue
 source=Path(inp['source']);archive=Path(inp['archive']);fixture=P/'candidate/t-gc2-worker-scheduler.c';exe=D/(inp['variant']+'-candidate');env=os.environ.copy();env['LUA_PATH']=str(source/'src/?.lua')+';'+str(P/'candidate/lib/?.lua')+';;'
 argv=['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-mcx16','-DLJ_GC2_TEST_HELPERS','-I'+str(source/'src'),str(fixture),str(archive),'-lm','-ldl','-pthread','-Wl,--wrap=pthread_create','-Wl,--wrap=pthread_join','-o',str(exe)]
 if run(inp['variant']+'-compile','compile',argv,source,env,[fixture,archive,exe])!=0:raise SystemExit(1)
 run(inp['variant']+'-candidate','runtime',['taskset','-c','0-15',str(exe)],source,env)
