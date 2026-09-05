from pathlib import Path
import json,subprocess,time,os,sys,hashlib,resource
P=Path('/tmp/lj-reclaim-fair-pass-20260905-kw8kfdam');A=P/'acceptance/mixed-v2';S=P/'candidate'
flags='-DLUA_USE_ASSERT -DLUA_USE_APICHECK -DLJ_GC2_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_FUNC_TEST_HELPERS -DLJ_UDATA_TEST_HELPERS -DLJ_STR_TEST_HELPERS'
env=os.environ.copy();env['LUA_PATH']=str(S/'src/?.lua')+';'+str(S/'tests/lib/?.lua')+';;'
resource.setrlimit(resource.RLIMIT_CORE,(0,0));rows=[]
def run(name,argv):
 start=time.monotonic();timed=False
 with (A/(name+'.stdout')).open('wb') as out,(A/(name+'.stderr')).open('wb') as err:
  try:r=subprocess.run(argv,cwd=S,env=env,stdout=out,stderr=err,timeout=60);code=r.returncode
  except subprocess.TimeoutExpired:timed=True;code=124
 row={'name':name,'argv':argv,'cwd':str(S),'LUA_PATH':env['LUA_PATH'],'timeout_seconds':60,'exit_code':code,'timed_out':timed,'seconds':time.monotonic()-start,'stdout':name+'.stdout','stderr':name+'.stderr'};rows.append(row)
 (A/'mixed-results.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps({k:row[k] for k in ['name','exit_code','timed_out','seconds']}),flush=True)
 return code
exe=A/'candidate-t-mixed-owner'
cmd=['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-I'+str(S/'src'),'-I'+str(S/'tests'),*flags.split(),str(A/'t-mixed-owner.c'),str(S/'src/libluajit.a'),'-Wl,-E','-lm','-ldl','-pthread','-o',str(exe)]
if run('mixed-compile',cmd):sys.exit(1)
(A/'mixed-identities.json').write_text(json.dumps({str(q):{'sha256':hashlib.sha256(q.read_bytes()).hexdigest(),'bytes':q.stat().st_size} for q in [A/'t-mixed-owner.c',P/'acceptance/v2/t-owner-defer.c',S/'src/libluajit.a',exe]},indent=2)+'\n')
for workers in [0,2]:run('mixed-workers-'+str(workers),[str(exe),str(workers),'1'])
