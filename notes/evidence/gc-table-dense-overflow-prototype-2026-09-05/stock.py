from pathlib import Path
import subprocess,os,signal,time,json
p=Path(__file__).parent;rows=[]
for mode in ['joff','jon']:
 cmd=['taskset','-c','0-15',str(p/'normal/src/luajit'),'-'+mode,'test.lua','--quiet']
 start=time.monotonic();q=subprocess.Popen(cmd,cwd=p/'normal/tests/stock/test',stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try:out,err=q.communicate(timeout=180);status='complete'
 except subprocess.TimeoutExpired:os.killpg(q.pid,signal.SIGKILL);out,err=q.communicate();status='timeout'
 row={'command':cmd,'cwd':str(p/'normal/tests/stock/test'),'seconds':time.monotonic()-start,'exit':q.returncode,'status':status,'stdout':out,'stderr':err};rows.append(row)
 (p/'stock-results.json').write_text(json.dumps(rows,indent=2)+'\n');print(mode,q.returncode,out[-500:],err,flush=True)
