from pathlib import Path
import csv,hashlib,json,os,resource,subprocess,threading,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent
meta=json.loads((p/'metadata.json').read_text());builds=json.loads((p/'builds.json').read_text())
sha=lambda f:hashlib.sha256(Path(f).read_bytes()).hexdigest()
for b in builds:assert sha(p/(b['variant']+'-'+b['name']))==b['binary_sha256']
for name,h in meta['source_sha256'].items():assert sha(p/name)==h
rows=[]
def save():
 (p/'measurements.json').write_text(json.dumps(rows,indent=2)+'\n')
for size in [64,256,4096]:
 for sample in range(1,6):
  for order,variant in enumerate(['baseline','candidate'] if sample%2 else ['candidate','baseline'],1):
   stem=f'hot-{size}-{sample}-{order}-{variant}'
   cmd=['taskset','-c',str(meta['cpu']),str(p/(variant+'-hot')),str(size),'500000']
   r=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
   (p/(stem+'.stdout')).write_text(r.stdout);(p/(stem+'.stderr')).write_text(r.stderr)
   row=dict(kind='hot',variant=variant,size=size,sample=sample,order=order,command=cmd,exit=r.returncode,stdout=r.stdout,stderr=r.stderr)
   if r.returncode==0:
    v=r.stdout.strip().split(',');assert len(v)==6
    row.update(operations=int(v[1]),cpu_seconds=float(v[2]),wall_seconds=float(v[3]),ns_per_op=float(v[4]),arenas=int(v[5]))
   rows.append(row);save();print(stem,r.returncode,flush=True);assert r.returncode==0,stem
fields=['VmSize','VmRSS','VmHWM','RssAnon','RssFile','Threads']
def read_status(pid):
 d={}
 for line in Path(f'/proc/{pid}/status').read_text().splitlines():
  if ':' in line:
   k,v=line.split(':',1)
   if k in fields:d[k]=int(v.split()[0])
 return d
for size,counts in [(4096,[256,2048,8192]),(16384,[64,512,2048])]:
 for count in counts:
  for sample in range(1,4):
   for order,variant in enumerate(['baseline','candidate'] if sample%2 else ['candidate','baseline'],1):
    stem=f'retention-{size}-{count}-{sample}-{order}-{variant}'
    cmd=['taskset','-c',str(meta['cpu']),str(p/(variant+'-retention')),str(count),str(size)]
    phases=[]
    with (p/(stem+'.stderr')).open('w') as err,(p/(stem+'.stdout')).open('w') as raw:
     proc=subprocess.Popen(cmd,stdout=subprocess.PIPE,stdin=subprocess.PIPE,stderr=err,text=True,bufsize=1)
     timer=threading.Timer(60,proc.kill);timer.daemon=True;timer.start()
     header=proc.stdout.readline();raw.write(header);columns=next(csv.reader([header]))
     for line in proc.stdout:
      raw.write(line);raw.flush();v=next(csv.reader([line]));assert len(v)==len(columns)
      phase=dict(zip(columns,v));phase.update({'proc_'+k:v for k,v in read_status(proc.pid).items()});phases.append(phase)
      assert phase['proc_VmRSS']<512*1024
      proc.stdin.write('go\n');proc.stdin.flush()
     code=proc.wait(timeout=10);timer.cancel()
    row=dict(kind='retention',variant=variant,size=size,count=count,sample=sample,order=order,command=cmd,exit=code,phases=phases,stderr=(p/(stem+'.stderr')).read_text())
    rows.append(row);save();print(stem,code,flush=True);assert code==0,stem
meta.update(status='timings complete',hot_processes=30,retention_processes=36,cpu8_released=True)
(p/'metadata.json').write_text(json.dumps(meta,indent=2)+'\n')
