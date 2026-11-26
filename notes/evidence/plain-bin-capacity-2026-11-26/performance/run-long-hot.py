from pathlib import Path
import hashlib,json,resource,subprocess,time
p=Path(__file__).resolve().parent
builds=json.loads((p/'builds.json').read_text());expected={b['variant']:b['binary_sha256'] for b in builds if b['name']=='hot'}
for v,h in expected.items():assert hashlib.sha256((p/(v+'-hot')).read_bytes()).hexdigest()==h
rows=[]
def limits():
 resource.setrlimit(resource.RLIMIT_CORE,(0,0));resource.setrlimit(resource.RLIMIT_AS,(1024*1024*1024,1024*1024*1024))
for sample in range(1,6):
 for order,variant in enumerate(['baseline','candidate'] if sample%2 else ['candidate','baseline'],1):
  stem=f'hot-long-4096-2000000-{sample}-{order}-{variant}'
  command=['taskset','-c','8',str(p/(variant+'-hot')),'4096','2000000']
  start=time.monotonic();r=subprocess.run(command,capture_output=True,text=True,timeout=60,preexec_fn=limits)
  (p/(stem+'.stdout')).write_text(r.stdout);(p/(stem+'.stderr')).write_text(r.stderr)
  row=dict(kind='hot_long',variant=variant,size=4096,sample=sample,order=order,command=command,exit=r.returncode,stdout=r.stdout,stderr=r.stderr,controller_seconds=time.monotonic()-start)
  rows.append(row);(p/'long-hot-measurements.json').write_text(json.dumps(rows,indent=2)+'\n');assert r.returncode==0,(stem,r.stderr)
  v=r.stdout.strip().split(',');assert len(v)==6
  row.update(operations=int(v[1]),cpu_seconds=float(v[2]),wall_seconds=float(v[3]),ns_per_op=float(v[4]),arenas=int(v[5]))
  (p/'long-hot-measurements.json').write_text(json.dumps(rows,indent=2)+'\n');print(stem,row['ns_per_op'],flush=True)
