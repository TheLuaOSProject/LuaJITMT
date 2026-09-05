from pathlib import Path
import json,subprocess,os,hashlib,time,resource
P=Path(__file__).resolve().parent;D=P/'matrix';D.mkdir();S=json.loads((P/'validation-inputs.json').read_text());rows=[]
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
 source=Path(inp['source']);archive=Path(inp['archive']);config=inp['configuration'];env=os.environ.copy();
 if config=='asan':env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
 for fixture_mode in ['candidate','baseline']:
  fixture=P/fixture_mode/'t-gc2-worker-scheduler.c';name=inp['variant']+'-'+config+'-'+fixture_mode;exe=D/name;env['LUA_PATH']=str(source/'src/?.lua')+';'+str(P/fixture_mode/'lib/?.lua')+';;'
  if config=='default':argv=['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-mcx16','-DLJ_GC2_TEST_HELPERS']
  elif config=='strict':argv=['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16']+inp['runtime_flags']
  else:argv=['clang','-std=gnu11','-O1','-g','-fsanitize=address','-fno-omit-frame-pointer','-Wall','-Wextra','-Werror','-mcx16']+inp['runtime_flags']
  argv.extend(['-I'+str(source/'src'),str(fixture),str(archive),'-lm','-ldl','-pthread','-Wl,--wrap=pthread_create','-Wl,--wrap=pthread_join','-o',str(exe)])
  if run(name+'-compile','compile',argv,source,env,[fixture,archive,exe])!=0:continue
  repeats=(10 if config=='default' else 3) if fixture_mode=='candidate' else (3 if config=='default' else 1)
  for i in range(repeats):run(name+'-'+str(i),'runtime',['taskset','-c','0-15',str(exe)],source,env)
