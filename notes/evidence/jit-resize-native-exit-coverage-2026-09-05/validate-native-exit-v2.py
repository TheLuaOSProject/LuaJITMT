from pathlib import Path
import subprocess,os,json,signal,time,hashlib
p=Path(__file__).parent;root=Path('/tmp/lj-gc-coalescing-final-20260905-0eig4rlf/normal');env=os.environ.copy()
for k in list(env):
 if k.startswith('LJ_M5_'):del env[k]
env['LUA_PATH']=str(root/'src/?.lua')+';'+str(root/'tests/lib/?.lua')+';;'
rows=[]
for variant,case,mode,cpus,n in [('positive','jitstore','-jon','0-15',50),('positive','jitread','-jon','0-15',20),('positive','jitstore,jitread,jititer','-jon','0-15',5),('positive','jitstore,jitread','-jon','0',5),('joff','jitstore','-joff','0-15',1),('joff','jitread','-joff','0-15',1),('no-worker-jit','jitstore','-jon','0-15',1),('no-worker-jit','jitread','-jon','0-15',1)]:
 for i in range(n):
  env['LJ_M5_TAB_RESIZE_STRESS_CASES']=case;f=p/('resize-no-worker-jit-v2.lua' if variant=='no-worker-jit' else 'resize-native-exit-v2.lua')
  cmd=['taskset','-c',cpus,str(root/'src/luajit'),mode,str(f)];start=time.monotonic();q=subprocess.Popen(cmd,env=env,cwd=root,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True)
  try:out,err=q.communicate(timeout=30);status='complete'
  except subprocess.TimeoutExpired:os.killpg(q.pid,signal.SIGKILL);out,err=q.communicate();status='timeout'
  good=(q.returncode==0) if variant=='positive' else (q.returncode==1 and 'workers did not execute native code' in err)
  r={'variant':variant,'case':case,'attempt':i+1,'command':cmd,'fixture_sha256':hashlib.sha256(f.read_bytes()).hexdigest(),'exit':q.returncode,'expected_result':good,'status':status,'seconds':time.monotonic()-start,'stdout':out,'stderr':err};rows.append(r)
  if not good or i==0:print(json.dumps(r),flush=True)
  (p/'native-exit-v2-validation.json').write_text(json.dumps(rows,indent=2)+'\n')
  if not good:break
print(json.dumps({'runs':len(rows),'expected_results':sum(r['expected_result'] for r in rows)}),flush=True)
