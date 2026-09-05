from pathlib import Path
import subprocess,os,signal,time,json,re,sys
p=Path(__file__).parent;pilot=len(sys.argv)>1 and sys.argv[1]=='pilot';rows=[]
def run(name,kind,variant,mode,cmd,env=None,pair=0):
 start=time.monotonic();q=subprocess.Popen(cmd,env=env,cwd=p/variant,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try:out,err=q.communicate(timeout=60 if pilot else 90);status='complete'
 except subprocess.TimeoutExpired:os.killpg(q.pid,signal.SIGKILL);out,err=q.communicate();status='timeout'
 row={'name':name,'kind':kind,'variant':variant,'mode':mode,'pair':pair,'command':cmd,'cwd':str(p/variant),'environment_overrides':{'BENCH_SCALE':env.get('BENCH_SCALE')} if env else {},'exit':q.returncode,'status':status,'seconds':time.monotonic()-start,'stdout':out,'stderr':err}
 if kind!='plan':row['data']=[json.loads(x) for x in out.splitlines() if x.startswith('{')]
 else:
  m=re.search(r'^'+re.escape(name)+r'\s+(\S+)\s+(\S+)\s*$',out,re.M)
  if m:row['plan_total_s'],row['ns_per_op']=map(float,m.groups())
 rows.append(row);(p/('cost-pilot.json' if pilot else 'cost-results.json')).write_text(json.dumps(rows,indent=2)+'\n')
 print(kind,name,variant,mode,pair,q.returncode,round(row['seconds'],3),flush=True)
 if q.returncode: print(out[-1500:],err[-2000:],flush=True);sys.exit(1)
 return row
for pair in range(1 if pilot else 7):
 order=[('base-normal','ordinary'),('normal','ordinary'),('normal','promoted')]
 if pair%2:order.reverse()
 for variant,mode in order:
  run(mode,'barrier',variant,'joff',['taskset','-c','31',str(p/(variant+'-cost')),'barrier',mode,'1000000','joff'],pair=pair)
for name in ['alloc_tables','tab_insert_newkey','closures_upval']:
 for mode in ['joff','jon']:
  for pair in range(1 if pilot else 7):
   variants=['base-normal','normal'] if pair%2==0 else ['normal','base-normal']
   for variant in variants:
    env=os.environ.copy();env['BENCH_SCALE']='0.02'
    run(name,'plan',variant,mode,['taskset','-c','31',str(p/variant/'src/luajit'),'-'+mode,str(p/variant/'plan/aux/bench/bench.lua'),name],env,pair)
for name in ['tables','insertion','closures','promoted_tables']:
 for mode in ['joff','jon']:
  for pair in range(1 if pilot else 3):
   variants=['base-normal','normal'] if pair%2==0 else ['normal','base-normal']
   for variant in variants:
    run(name,'memory',variant,mode,['taskset','-c','31',str(p/(variant+'-cost')),'memory',name,'20000',mode],pair=pair)
