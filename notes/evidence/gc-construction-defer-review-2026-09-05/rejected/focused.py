from pathlib import Path
import json,subprocess,time,os,hashlib,resource,sys
P=Path('/tmp/lj-reclaim-owner-defer-20260905-gwiiudxk')
variant=sys.argv[1]
S=P/'candidate' if variant=='candidate' else Path('/tmp/lj-gc-auto-stop-overlap-20260905-y4h4cc8a/controlextrahelpers')
flags='-DLUA_USE_ASSERT -DLUA_USE_APICHECK -DLJ_GC2_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_FUNC_TEST_HELPERS -DLJ_UDATA_TEST_HELPERS -DLJ_STR_TEST_HELPERS'
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
env=os.environ.copy();env['LUA_PATH']=str(S/'src/?.lua')+';'+str(S/'tests/lib/?.lua')+';;'
results=[]
def ident(q):q=Path(q);return {'sha256':hashlib.sha256(q.read_bytes()).hexdigest(),'bytes':q.stat().st_size}
def run(name,argv,bound):
 t=time.monotonic();timed=False
 with (P/(name+'.stdout')).open('wb') as out,(P/(name+'.stderr')).open('wb') as err:
  try:r=subprocess.run(argv,cwd=S,env=env,stdout=out,stderr=err,timeout=bound);code=r.returncode
  except subprocess.TimeoutExpired:code=124;timed=True
 row={'name':name,'argv':argv,'cwd':str(S),'LUA_PATH':env['LUA_PATH'],'timeout_seconds':bound,'exit_code':code,'timed_out':timed,'seconds':time.monotonic()-t,'stdout':name+'.stdout','stderr':name+'.stderr'}
 results.append(row);(P/(variant+'-focused-results.json')).write_text(json.dumps(results,indent=2)+'\n')
 print(json.dumps({k:row[k] for k in ['name','exit_code','timed_out','seconds']}),flush=True)
 return code
if variant=='candidate':
 if run('candidate-build',['make','-C','src','-j4','BUILDMODE=static','XCFLAGS='+flags,'TARGET_STRIP=:'],120):sys.exit(1)
exe=P/(variant+'-t-func-construction-anchor')
args=['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-I'+str(S/'src'),*flags.split(),str(S/'tests/t-func-construction-anchor.c'),str(S/'src/libluajit.a'),'-Wl,-E','-lm','-ldl','-pthread','-o',str(exe)]
if run(variant+'-compile',args,60):sys.exit(1)
(P/(variant+'-focused-identities.json')).write_text(json.dumps({str(q):ident(q) for q in [S/'tests/t-func-construction-anchor.c',S/'src/libluajit.a',exe]},indent=2)+'\n')
run(variant+'-fixture',[str(exe)],60)
