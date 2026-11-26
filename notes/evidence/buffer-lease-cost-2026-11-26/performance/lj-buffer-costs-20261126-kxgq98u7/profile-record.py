from pathlib import Path
import subprocess,json
p=Path(__file__).resolve().parent
meta=[]
for variant in ['current','baseline']:
 for work,size,count,rounds,capacity in [('append',1,1000000,30 if variant=='current' else 600,1048576),('resetget',1024,50000,10 if variant=='current' else 80,1024)]:
  stem=f'user-record-{variant}-{work}-{size}'
  cmd=['taskset','-c','8','perf','record','--all-user','--user-callchains','-o',str(p/(stem+'.data')),'--call-graph','dwarf,8192','-F','499','-e','cpu_core/cycles/u','--',str(p/(variant+'-luajit')),str(p/'workload.lua'),work,str(size),str(count),str(rounds),str(capacity)]
  r=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
  (p/(stem+'.stdout')).write_text(r.stdout);(p/(stem+'.stderr')).write_text(r.stderr)
  if r.returncode:raise SystemExit(stem+': '+r.stderr)
  for kind,args in [('flat',['--no-children','-F','overhead,symbol']),('callgraph',['-g','graph,0.5,caller'])]:
   cmd=['perf','report','-i',str(p/(stem+'.data')),'--stdio','--percent-limit','0.5']+args
   r=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
   (p/(stem+'-'+kind+'.txt')).write_text(r.stdout+r.stderr)
   if r.returncode:raise SystemExit(stem+' report failed')
  meta.append(dict(stem=stem,command=cmd,variant=variant,workload=work,size=size,operations=count*rounds))
  print(stem,flush=True)
(p/'sampling-metadata.json').write_text(json.dumps(meta,indent=2)+'\n')
print('sampling complete',flush=True)
