from pathlib import Path
import json,subprocess,time,os,hashlib,resource
P=Path(__file__).resolve().parent;A=P/'results';setup=json.loads((P/'setup.json').read_text());rows=[]
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
def run(name,argv,cwd,env,kind):
 start=time.monotonic();timed=False
 with (A/(name+'.stdout')).open('wb') as out,(A/(name+'.stderr')).open('wb') as err:
  try:r=subprocess.run(argv,cwd=cwd,env=env,stdout=out,stderr=err,timeout=60);code=r.returncode
  except subprocess.TimeoutExpired:code=124;timed=True
 row=dict(name=name,kind=kind,argv=argv,cwd=str(cwd),environment={'LUA_PATH':env.get('LUA_PATH','')},timeout_seconds=60,exit_code=code,timed_out=timed,seconds=time.monotonic()-start,stdout=name+'.stdout',stderr=name+'.stderr');rows.append(row)
 (A/'results.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps({k:row[k] for k in ['name','kind','exit_code','timed_out','seconds']}),flush=True)
 return code
for variant in ['combined','baseline']:
 source=Path(setup['runtime_sources'][variant]['source']);archive=Path(setup['runtime_sources'][variant]['runtime_archive'])
 env=os.environ.copy();env['LUA_PATH']=str(source/'src/?.lua')+';'+str(P/'fixture/lib/?.lua')+';;'
 for mode in ['untouched','observer']:
  exe=A/(variant+'-'+mode)
  argv=['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-mcx16','-DLJ_GC2_TEST_HELPERS','-I'+str(source/'src'),str(P/'fixture/t-gc2-worker-scheduler.c')]
  if mode=='observer':argv.append(str(P/'diagnostic/abort-observer.c'))
  argv.extend([str(archive),'-lm','-ldl','-pthread','-Wl,--wrap=pthread_create','-Wl,--wrap=pthread_join'])
  if mode=='observer':argv.append('-Wl,--wrap=abort')
  argv.extend(['-o',str(exe)])
  if run(variant+'-'+mode+'-compile',argv,source,env,'compile')!=0:raise SystemExit(1)
  files=[P/'fixture/t-gc2-worker-scheduler.c',archive,exe]
  if mode=='observer':files.append(P/'diagnostic/abort-observer.c')
  (A/(variant+'-'+mode+'-identities.json')).write_text(json.dumps({str(f):{'sha256':hashlib.sha256(f.read_bytes()).hexdigest(),'bytes':f.stat().st_size} for f in files},indent=2)+'\n')
  run(variant+'-'+mode,['taskset','-c','0-15',str(exe)],source,env,'runtime')
