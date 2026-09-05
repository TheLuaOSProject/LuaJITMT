from pathlib import Path
import subprocess,os,signal,time,json,sys
p=Path(__file__).parent;pilot=len(sys.argv)>1 and sys.argv[1]=='pilot';rows=[]
def run(variant,kind,size,touch,pair,cmd):
 start=time.monotonic();q=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try:out,err=q.communicate(timeout=60);status='complete'
 except subprocess.TimeoutExpired:os.killpg(q.pid,signal.SIGKILL);out,err=q.communicate();status='timeout'
 data=[json.loads(x) for x in out.splitlines() if x.startswith('{')]
 row={'variant':variant,'kind':kind,'size':size,'touch':touch,'pair':pair,'command':cmd,'exit':q.returncode,'status':status,'seconds':time.monotonic()-start,'stdout':out,'stderr':err,'data':data};rows.append(row);(p/('cost-pilot.json' if pilot else 'cost-results.json')).write_text(json.dumps(rows,indent=2)+'\n');print(variant,kind,size,touch,pair,q.returncode,round(row['seconds'],4),flush=True)
 if q.returncode:print(out[-1500:],err,flush=True);raise SystemExit(1)
for size in ([20000,65393] if pilot else [20000,65392,65393,65408,65409]):
 for kind,touch in [('plain','untouched'),('plain','payload'),('traversable','untouched'),('traversable','payload'),('traversable','wide')]:
  for pair in range(1 if pilot else 7):
   variants=['calloc-normal','tail-normal'] if pair%2==0 else ['tail-normal','calloc-normal']
   for variant in variants:run(variant,kind,size,touch,pair,['taskset','-c','31',str(p/(variant+'-cost')),'standalone',str(size),'512','1' if kind=='traversable' else '0',touch])
for size in ([20000,65393] if pilot else [20000,65392,65393,65408,65409]):
 for pair in range(1 if pilot else 3):
  variants=['calloc-normal','tail-normal'] if pair%2==0 else ['tail-normal','calloc-normal']
  for variant in variants:run(variant,'runtime',size,'payload',pair,['taskset','-c','31',str(p/(variant+'-cost')),'runtime',str(size),'256'])
