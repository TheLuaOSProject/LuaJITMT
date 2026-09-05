from pathlib import Path
import json,subprocess,os,hashlib,time,resource
P=Path(__file__).resolve().parent;D=P/'oracle';D.mkdir();S=json.loads((P/'validation-inputs.json').read_text());rows=[]
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
 source=Path(inp['source']);archive=Path(inp['archive']);fixture=P/'candidate/t-gc2-worker-scheduler.c';env=os.environ.copy();env['LUA_PATH']=str(source/'src/?.lua')+';'+str(P/'candidate/lib/?.lua')+';;'
 for mode in ['observer','negative-fault','positive-fault']:
  name=inp['variant']+'-'+mode;exe=D/name
  argv=['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16','-DLJ_GC2_TEST_HELPERS','-I'+str(source/'src'),str(fixture)]
  observer=[];wrap=[]
  if mode=='observer':
   observer=[P/'observers/boundary-observer.c',P/'observers/prepare-observer.c'];wrap=['-Wl,--wrap=lj_gc2_sweep_bridge_ready','-Wl,--wrap=lj_gc2_sweep_to_idle','-Wl,--wrap=lj_safepoint_handshake']
  else:
   observer=[P/'observers/close-fault.c'];wrap=['-Wl,--wrap=lj_gc2_sweep_to_idle'];argv.append('-DDIAG_CLOSE_FAULT='+('1' if mode=='negative-fault' else '2'))
  argv.extend([str(f) for f in observer]);argv.extend([str(archive),'-lm','-ldl','-pthread','-Wl,--wrap=pthread_create','-Wl,--wrap=pthread_join']);argv.extend(wrap);argv.extend(['-o',str(exe)])
  if run(name+'-compile','compile',argv,source,env,[fixture,archive,exe]+observer)!=0:raise SystemExit(1)
  run(name,'runtime',['taskset','-c','0-15',str(exe)],source,env)
