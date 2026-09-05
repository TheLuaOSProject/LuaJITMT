from pathlib import Path
import json,subprocess,os,hashlib,time,resource,signal
P=Path(__file__).resolve().parent;D=P/'gdb3';S=json.loads((P/'setup.json').read_text());rows=[]
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
def save(row):
 rows.append(row);(D/'results.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps({k:row[k] for k in ['name','kind','exit_code','seconds'] if k in row}),flush=True)
for variant in ['combined','baseline']:
 s=S['runtime_sources'][variant];source=Path(s['source']);archive=Path(s['runtime_archive']);fixture=P/'fixture/t-gc2-worker-scheduler.c';observer=D/'abort-stop-observer.c';exe=D/(variant+'-stop-observer')
 argv=['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16','-DLJ_GC2_TEST_HELPERS','-I'+str(source/'src'),str(fixture),str(observer),str(D/'ready-observer.c'),str(archive),'-lm','-ldl','-pthread','-Wl,--wrap=pthread_create','-Wl,--wrap=pthread_join','-Wl,--wrap=abort','-Wl,--wrap=lj_gc2_sweep_bridge_ready','-o',str(exe)]
 start=time.monotonic();r=subprocess.run(argv,cwd=source,capture_output=True);(D/(variant+'-compile.stdout')).write_bytes(r.stdout);(D/(variant+'-compile.stderr')).write_bytes(r.stderr);save(dict(name=variant+'-compile',kind='compile',argv=argv,cwd=str(source),exit_code=r.returncode,seconds=time.monotonic()-start,identities={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in [archive,fixture,observer,D/'ready-observer.c',exe]}));assert r.returncode==0
 env=os.environ.copy();env['LUA_PATH']=str(source/'src/?.lua')+';'+str(P/'fixture/lib/?.lua')+';;'
 for i in range(8):
  name=variant+'-'+str(i);argv=['taskset','-c','0-15',str(exe)];start=time.monotonic();stopped=False;timed=False
  with (D/(name+'.stdout')).open('wb') as out,(D/(name+'.stderr')).open('wb') as err:
   proc=subprocess.Popen(argv,cwd=source,env=env,stdout=out,stderr=err)
   while proc.poll() is None:
    status=Path('/proc/'+str(proc.pid)+'/status').read_text()
    if any(line.startswith('State:') and 'T (stopped)' in line for line in status.splitlines()):
     stopped=True;break
    if time.monotonic()-start>60:timed=True;proc.kill();break
    time.sleep(0.001)
   if stopped:
    ga=['gdb','-q','-batch','-x',str(D/'commands.gdb'),str(exe),'-p',str(proc.pid)];gt=time.monotonic()
    with (D/(name+'-gdb.stdout')).open('wb') as gout,(D/(name+'-gdb.stderr')).open('wb') as gerr:
     try:gr=subprocess.run(ga,cwd=source,stdout=gout,stderr=gerr,timeout=30);gc=gr.returncode
     except subprocess.TimeoutExpired:gc=124
    save(dict(name=name+'-gdb',kind='gdb',argv=ga,cwd=str(source),exit_code=gc,seconds=time.monotonic()-gt,timeout_seconds=30))
    os.kill(proc.pid,signal.SIGCONT)
   try:rc=proc.wait(timeout=10)
   except subprocess.TimeoutExpired:proc.kill();rc=proc.wait();timed=True
  save(dict(name=name,kind='runtime',argv=argv,cwd=str(source),environment={'LUA_PATH':env['LUA_PATH']},exit_code=rc,seconds=time.monotonic()-start,timeout_seconds=60,timed_out=timed,abort_stop_observed=stopped))
  if stopped:break
