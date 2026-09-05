from pathlib import Path
import os,subprocess,json,time,signal,hashlib
r=Path(__file__).resolve().parent;rows=[]
for v in ['fix-normal','fix-assert']:
 t=r/v;env=os.environ.copy();env['LUA_PATH']=str(t/'src/?.lua')+';'+str(t/'tests/lib/?.lua')+';;'
 for mode in ['replace','reentrant','inplace']:
  name=v+'-'+mode;cmd=['taskset','-c','0-15',str(t/'src/luajit'),'-jon',str(r/'basemt-native.lua'),mode]
  st=time.monotonic();q=subprocess.Popen(cmd,cwd=t,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
  try:out,err=q.communicate(timeout=20);status='complete'
  except subprocess.TimeoutExpired:os.killpg(q.pid,signal.SIGKILL);out,err=q.communicate();status='timeout'
  (r/(name+'.stdout')).write_text(out);(r/(name+'.stderr')).write_text(err);rows.append({'name':name,'command':cmd,'cwd':str(t),'environment_overrides':{'LUA_PATH':env['LUA_PATH']},'exit':q.returncode,'status':status,'seconds':time.monotonic()-st,'stdout':out,'stderr':err});print(name,q.returncode,out,err,flush=True)
(r/'guard-original-fixture-results.json').write_text(json.dumps(rows,indent=2)+'\n')
