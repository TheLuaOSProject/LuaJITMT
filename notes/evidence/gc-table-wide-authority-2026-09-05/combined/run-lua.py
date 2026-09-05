from pathlib import Path
import subprocess,os,signal,time,json,sys,resource
P=Path(__file__).resolve().parent;variant=sys.argv[1];T=P/variant;rows=[]
baseenv=os.environ.copy();baseenv.pop('ASAN_OPTIONS',None)
baseenv['LUA_PATH']=str(T/'tests/lib/?.lua')+';'+str(T/'src/?.lua')+';'+str(T/'src/?/init.lua')+';;'
if variant=='asan':baseenv['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
def run(name,cmd,cwd=None,extra=None,timeout=90):
 env=baseenv.copy();env.update(extra or {});cwd=cwd or T
 start=time.monotonic();q=subprocess.Popen(cmd,cwd=cwd,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try:out,err=q.communicate(timeout=timeout);status='complete'
 except subprocess.TimeoutExpired:os.killpg(q.pid,signal.SIGKILL);out,err=q.communicate();status='timeout'
 row={'name':name,'command':cmd,'cwd':str(cwd),'environment':{k:env[k] for k in ['ASAN_OPTIONS','LUA_PATH']+list(extra or {}) if k in env},'exit':q.returncode,'status':status,'seconds':time.monotonic()-start,'stdout':out,'stderr':err}
 rows.append(row);(P/(variant+'-lua.json')).write_text(json.dumps(rows,indent=2)+'\n');print(name,q.returncode,round(row['seconds'],3),flush=True)
 if q.returncode:print(out[-3000:],err[-5000:],flush=True)
 return q.returncode
def lua(name,script,mode='jon',extra=None,args=None,timeout=90):
 return run(name+'-'+mode,['taskset','-c','0-15',str(T/'src/luajit'),'-'+mode,str(script)]+(args or []),extra=extra,timeout=timeout)
for mode in ['joff','jon']:
 run('stock-'+mode,['taskset','-c','0-15',str(T/'src/luajit'),'-'+mode,'test.lua','--quiet'],cwd=T/'tests/stock/test',timeout=120)
 lua('cdata-capture',T/'tests/t-meta-cdata-capture.lua',mode)
 lua('cdata-native-guards-eight-modes',T/'tests/t-jit-cdata-basemt-guards.lua',mode)
 lua('weak-modes',T/'tests/t-weak-modes.lua',mode)
 lua('weakmeta-bridge',T/'tests/t-gc2-weakmeta-bridge.lua',mode)
 lua('finalizer-peer-collect',T/'tests/t-gc2-finalizer-peer-collect.lua',mode,timeout=40)
 lua('finalizer-spawn-live',T/'tests/t-m8-finalizer-spawn-live.lua',mode,timeout=40)
 lua('ffi-finreg',T/'tests/t-ffi-gc-finreg.lua',mode,args=['3','72'])
 # Preserve canonical object counts and worker counts; select relevant cases.
 extra={'LJ_M5_TAB_RESIZE_STRESS_CASES':'weak,gcmark,gckey,weakkey,weakmeta,finalizer,metatable,len,traversal,nextchurn,nextinvalid,tableclear,tablelib,tablelibshift,metadispatch'}
 lua('concurrent-table-meta-weak-finalizer',T/'tests/t-tab-resize-stress.lua',mode,extra=extra,timeout=120)
for name in ['m6_jit_mt_activation_flush','m6_jit_gcworkers_activation_flush']:
 lua(name,P/(name+'.lua'),timeout=45)
lua('concurrent-table-jit',T/'tests/t-tab-resize-stress.lua',extra={'LJ_M5_TAB_RESIZE_STRESS_CASES':'jitstore,jitread,jititer,weakfinjit'},timeout=120)
print('Lua run complete',variant,flush=True)
