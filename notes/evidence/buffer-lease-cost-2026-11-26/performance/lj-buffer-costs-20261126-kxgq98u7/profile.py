from pathlib import Path
import subprocess,json
p=Path(__file__).resolve().parent
cases=[('append',1,1000000,10,1048576),('resetget',64,200000,5,64),('resetget',1024,100000,5,1024)]
meta=[]
for sample in [1,2,3]:
 for work,size,count,rounds,capacity in cases:
  for variant in (['baseline','current'] if sample%2 else ['current','baseline']):
   stem=f'stat-{sample}-{variant}-{work}-{size}'
   cmd=['taskset','-c','8','perf','stat','-x',',','-o',str(p/(stem+'.csv')),'-e','cpu_core/cycles/u,cpu_core/instructions/u,cpu_core/mem_inst_retired.lock_loads/u','--',str(p/(variant+'-luajit')),str(p/'workload.lua'),work,str(size),str(count),str(rounds),str(capacity)]
   r=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
   (p/(stem+'.stdout')).write_text(r.stdout);(p/(stem+'.stderr')).write_text(r.stderr)
   if r.returncode:raise SystemExit(stem+': '+r.stderr)
   meta.append(dict(stem=stem,command=cmd,variant=variant,workload=work,size=size,operations=count*rounds))
   print(stem,flush=True)
for variant in ['current','baseline']:
 for work,size,count,rounds,capacity in [('append',1,1000000,30 if variant=='current' else 600,1048576),('resetget',1024,100000,20 if variant=='current' else 80,1024)]:
  stem=f'record-{variant}-{work}-{size}'
  cmd=['taskset','-c','8','perf','record','-o',str(p/(stem+'.data')),'--call-graph','dwarf,8192','-F','499','-e','cpu_core/cycles/u','--',str(p/(variant+'-luajit')),str(p/'workload.lua'),work,str(size),str(count),str(rounds),str(capacity)]
  r=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
  (p/(stem+'.stdout')).write_text(r.stdout);(p/(stem+'.stderr')).write_text(r.stderr)
  if r.returncode:raise SystemExit(stem+': '+r.stderr)
  cmd=['perf','report','-i',str(p/(stem+'.data')),'--stdio','--no-children','-F','overhead,symbol','--percent-limit','0.5']
  r=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
  (p/(stem+'-report.txt')).write_text(r.stdout+r.stderr)
  if r.returncode:raise SystemExit(stem+' report failed')
  meta.append(dict(stem=stem,variant=variant,workload=work,size=size,operations=count*rounds))
  print(stem,flush=True)
(p/'profile-metadata.json').write_text(json.dumps(meta,indent=2)+'\n')
print('profile complete',flush=True)
