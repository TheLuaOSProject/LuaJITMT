from pathlib import Path
import subprocess,time,json,os,hashlib,signal,statistics
r=Path(__file__).resolve().parent;outdir=r/'cost';outdir.mkdir();rows=[]
workloads={'ffi_struct':['aux/bench/bench.lua','ffi_struct'],'constructor_sink':[str(r/'constructor-cost.lua'),'sink'],'constructor_nosink':[str(r/'constructor-cost.lua'),'nosink']}
for pair in range(7):
 for workload,args in workloads.items():
  for variant in (('base-normal','fix-normal') if pair%2==0 else ('fix-normal','base-normal')):
   tree=r/variant;name=f'{workload}-pair{pair+1}-{variant}';exe=tree/'src/luajit'
   env=os.environ.copy();override={'LUA_PATH':str(tree/'src/?.lua')+';;','BENCH_SCALE':'1'};env.update(override)
   cmd=['taskset','-c','31',str(exe),'-jon']+args
   t=time.monotonic();p=subprocess.Popen(cmd,cwd=tree,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
   try:out,err=p.communicate(timeout=45);status='complete'
   except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);out,err=p.communicate();status='timeout'
   (outdir/(name+'.stdout')).write_text(out);(outdir/(name+'.stderr')).write_text(err)
   if workload=='ffi_struct':samples=[float(line.split()[1]) for line in out.splitlines() if line.startswith('ffi_struct ')]
   else:samples=[float(line.split()[2]) for line in out.splitlines() if line.startswith('sample ')]
   row={'name':name,'workload':workload,'pair':pair+1,'variant':variant,'command':cmd,'cwd':str(tree),'environment_overrides':override,'exit':p.returncode,'status':status,'wall_seconds':time.monotonic()-t,'samples_cpu_seconds':samples,'best_cpu_seconds':min(samples) if samples else None,'runtime_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'fixture_sha256':hashlib.sha256((tree/args[0]).read_bytes()).hexdigest(),'stdout':str(outdir/(name+'.stdout')),'stderr':str(outdir/(name+'.stderr'))}
   rows.append(row);(r/'guard-cost-results.json').write_text(json.dumps(rows,indent=2)+'\n')
   print(name,p.returncode,row['best_cpu_seconds'],round(row['wall_seconds'],3),flush=True)
   assert p.returncode==0 and samples,(name,err)
summary={}
for workload in workloads:
 paired=[]
 for pair in range(1,8):
  old=next(x for x in rows if x['workload']==workload and x['pair']==pair and x['variant']=='base-normal')['best_cpu_seconds']
  new=next(x for x in rows if x['workload']==workload and x['pair']==pair and x['variant']=='fix-normal')['best_cpu_seconds']
  paired.append({'pair':pair,'baseline':old,'fix':new,'change_percent':100*(new/old-1)})
 summary[workload]={'pairs':paired,'median_paired_change_percent':statistics.median(x['change_percent'] for x in paired),'baseline_median_seconds':statistics.median(x['baseline'] for x in paired),'fix_median_seconds':statistics.median(x['fix'] for x in paired)}
(r/'guard-cost-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2),flush=True)
