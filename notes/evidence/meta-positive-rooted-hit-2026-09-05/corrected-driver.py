from pathlib import Path
import os,subprocess,time,signal,json
p=Path(__file__).parent;records=[]
flags=['-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLUA_USE_ASSERT']
def run(label,cmd,cwd,limit=45,extra=None,expected_negative=False):
 env=os.environ.copy();env['LUA_PATH']=str(p/'normal/src/?.lua')+';'+str(p/'normal/tests/lib/?.lua')+';;'
 if extra:env.update(extra)
 start=time.monotonic()
 with (p/(label+'.stdout')).open('w') as out,(p/(label+'.stderr')).open('w') as err:
  proc=subprocess.Popen(cmd,cwd=cwd,env=env,stdout=out,stderr=err,start_new_session=True)
  try:proc.wait(timeout=limit);status='complete'
  except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait();status='timeout'
 stderr=(p/(label+'.stderr')).read_text();stdout=(p/(label+'.stdout')).read_text()
 ok=(status=='complete' and ((proc.returncode!=0 and 'bounded_hit' in stderr and 'outword' in stderr) if expected_negative else proc.returncode==0))
 r={'label':label,'command':cmd,'cwd':str(cwd),'environment':{'LUA_PATH':env['LUA_PATH'],**(extra or {})},'seconds':time.monotonic()-start,'limit_seconds':limit,'status':status,'exit':proc.returncode,'expected_negative':expected_negative,'expected_result':ok,'stdout':stdout,'stderr':stderr};records.append(r);(p/'corrected-driver-validation.json').write_text(json.dumps(records,indent=2)+'\n');print(label,proc.returncode,ok,flush=True)
 return ok
root=p/'normal'
for mode in ['-joff','-jon']:
 run('corrected-stock-'+mode[1:],['taskset','-c','0-15',str(root/'src/luajit'),mode,'test.lua','--quiet'],root/'tests/stock/test',60)
for label,mode,case in [('general','-joff','weak,gcmark,gckey,weakkey,weakmeta,finalizer,metatable,len,traversal,nextchurn,nextinvalid,tableclear,tablelib,tablelibshift,metadispatch'),('jit','-jon','jitstore,jitread,jititer'),('weakfin','-jon','weakfinjit'),('remote','-jon','remotejitgc')]:
 run('corrected-resize-'+label,['taskset','-c','0-15',str(root/'src/luajit'),mode,str(root/'tests/t-tab-resize-stress.lua')],root,60,{'LJ_M5_TAB_RESIZE_STRESS_CASES':case})
print('expected',sum(r['expected_result'] for r in records),'of',len(records),flush=True)
