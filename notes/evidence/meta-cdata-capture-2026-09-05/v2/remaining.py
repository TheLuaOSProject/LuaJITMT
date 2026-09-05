from pathlib import Path
import subprocess,os,time,signal,json,re
p=Path(__file__).parent;records=[]
def run(label,cmd,cwd,limit=60,extra=None):
 env=os.environ.copy();env['LUA_PATH']=str(p/'normal/src/?.lua')+';'+str(p/'normal/tests/lib/?.lua')+';;'
 if extra:env.update(extra)
 start=time.monotonic()
 with (p/(label+'.stdout')).open('w') as out,(p/(label+'.stderr')).open('w') as err:
  proc=subprocess.Popen(cmd,cwd=cwd,env=env,stdout=out,stderr=err,start_new_session=True)
  try:proc.wait(timeout=limit);status='complete'
  except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait();status='timeout'
 rec={'label':label,'command':cmd,'cwd':str(cwd),'environment':{'LUA_PATH':env['LUA_PATH'],**(extra or {})},'exit':proc.returncode,'status':status,'seconds':time.monotonic()-start,'limit_seconds':limit};records.append(rec);(p/'remaining-validation.json').write_text(json.dumps(records,indent=2)+'\n');print(json.dumps(rec),flush=True)
 assert rec['exit']==0 and rec['status']=='complete',rec
root=p/'normal'
run('resize-metadispatch',['taskset','-c','0-15',str(root/'src/luajit'),'-jon',str(root/'tests/t-tab-resize-stress.lua')],root,60,{'LJ_M5_TAB_RESIZE_STRESS_CASES':'metatable,metadispatch,weakmeta'})
root=p/'canonical'
run('canonical',['taskset','-c','0-15',str(p/'normal/src/luajit'),str(root/'tools/test.lua'),'m5_meta_rooted_chain','m5_x64_rooted_reads'],root,180,{'LJ_TEST_ROOT':str(root),'JOBS':'4'})
