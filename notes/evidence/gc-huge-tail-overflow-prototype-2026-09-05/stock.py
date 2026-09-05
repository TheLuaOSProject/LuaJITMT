from pathlib import Path
import subprocess,signal,time,json,os
p=Path(__file__).parent;rows=[]
for variant in ['calloc-normal','tail-normal']:
 for mode in ['joff','jon']:
  cmd=['taskset','-c','0-15',str(p/variant/'src/luajit'),'-'+mode,'test.lua','--quiet'];cwd=p/variant/'tests/stock/test'
  start=time.monotonic();q=subprocess.Popen(cmd,cwd=cwd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
  try:out,err=q.communicate(timeout=180);status='complete'
  except subprocess.TimeoutExpired:os.killpg(q.pid,signal.SIGKILL);out,err=q.communicate();status='timeout'
  rows.append({'command':cmd,'cwd':str(cwd),'exit':q.returncode,'status':status,'seconds':time.monotonic()-start,'stdout':out,'stderr':err});(p/'stock-results.json').write_text(json.dumps(rows,indent=2)+'\n');print(variant,mode,q.returncode,out,err,flush=True)
