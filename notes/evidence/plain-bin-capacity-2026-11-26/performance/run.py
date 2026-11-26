from pathlib import Path
import csv,hashlib,json,os,resource,subprocess,threading,time
p=Path(__file__).resolve().parent
meta=json.loads((p/'metadata.json').read_text());builds=json.loads((p/'builds.json').read_text())
sha=lambda f:hashlib.sha256(Path(f).read_bytes()).hexdigest()
for b in builds:assert sha(p/(b['variant']+'-'+b['name']))==b['binary_sha256']
for name,h in meta['source_sha256'].items():assert sha(p/name)==h
for v,h in meta['luajit_sha256'].items():assert sha(p/(v+'-luajit'))==h
rows=[]
def limits():
 resource.setrlimit(resource.RLIMIT_CORE,(0,0))
 resource.setrlimit(resource.RLIMIT_AS,(1024*1024*1024,1024*1024*1024))
def save():
 (p/'measurements.json').write_text(json.dumps(rows,indent=2)+'\n')
def variants(sample):
 return ['baseline','candidate'] if sample%2 else ['candidate','baseline']
def simple(kind,args,settings,samples):
 for sample in range(1,samples+1):
  for order,variant in enumerate(variants(sample),1):
   stem='-'.join([kind]+[str(x) for x in args]+[str(sample),str(order),variant])
   command=['taskset','-c',str(meta['cpu']),str(p/(variant+'-'+kind))]+[str(x) for x in args]
   start=time.monotonic();r=subprocess.run(command,capture_output=True,text=True,timeout=60,preexec_fn=limits)
   (p/(stem+'.stdout')).write_text(r.stdout);(p/(stem+'.stderr')).write_text(r.stderr)
   row=dict(kind=kind,variant=variant,sample=sample,order=order,command=command,exit=r.returncode,stdout=r.stdout,stderr=r.stderr,controller_seconds=time.monotonic()-start,**settings)
   rows.append(row);save();assert r.returncode==0,(stem,r.stderr)
   v=r.stdout.strip().split(',')
   if kind=='hot':
    assert len(v)==6
    row.update(operations=int(v[1]),cpu_seconds=float(v[2]),wall_seconds=float(v[3]),ns_per_op=float(v[4]),arenas=int(v[5]))
   else:
    assert len(v)==12
    row.update(free_cpu=float(v[2]),free_wall=float(v[3]),refill_cpu=float(v[4]),refill_wall=float(v[5]),arenas_before=int(v[6]),arenas_after=int(v[7]),retained_targets=int(v[8]),free_cells=int(v[9]),expected_cells=int(v[10]),reused=int(v[11]))
   save();print(stem,'PASS',flush=True)
fields=['VmSize','VmRSS','VmHWM','VmData','RssAnon','RssFile','VmSwap','Threads']
def read_status(pid):
 d={}
 for line in Path(f'/proc/{pid}/status').read_text().splitlines():
  if ':' in line:
   k,v=line.split(':',1)
   if k in fields:d[k]=int(v.split()[0])
 return d
def staged(kind,args,settings,samples):
 for sample in range(1,samples+1):
  for order,variant in enumerate(variants(sample),1):
   stem='-'.join([kind]+[str(x) for x in args]+[str(sample),str(order),variant])
   binary=variant+'-'+('retention' if kind=='retention' else 'luajit')
   command=['taskset','-c',str(meta['cpu']),str(p/binary)]
   command+=([str(x) for x in args] if kind=='retention' else [str(p/'growth.lua'),'auto',str(sample)]+[str(x) for x in args])
   phases=[];start=time.monotonic()
   with (p/(stem+'.stderr')).open('w') as err,(p/(stem+'.stdout')).open('w') as raw:
    proc=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,text=True,bufsize=1,preexec_fn=limits)
    timer=threading.Timer(60,proc.kill);timer.daemon=True;timer.start()
    header=proc.stdout.readline();raw.write(header);columns=next(csv.reader([header]))
    for line in proc.stdout:
     raw.write(line);raw.flush();v=next(csv.reader([line]));assert len(v)==len(columns),(stem,line)
     phase=dict(zip(columns,v));phase.update({'proc_'+k:v for k,v in read_status(proc.pid).items()});phases.append(phase)
     assert phase['proc_VmRSS']<512*1024
     proc.stdin.write('go\n');proc.stdin.flush()
    code=proc.wait(timeout=10);timer.cancel()
   row=dict(kind=kind,variant=variant,sample=sample,order=order,command=command,exit=code,phases=phases,stderr=(p/(stem+'.stderr')).read_text(),controller_seconds=time.monotonic()-start,**settings)
   rows.append(row);save();print(stem,code,flush=True);assert code==0,(stem,row['stderr'])
for size in meta['hot_sizes']:simple('hot',[size,meta['hot_operations']],{'size':size},meta['hot_samples'])
for count in meta['bulk_counts']:
 for flag in meta['bulk_kinds']:simple('bulk',[count,flag],{'count':count,'traversable':flag},meta['bulk_samples'])
for size,count in meta['retention_cases']:staged('retention',[count,size],{'size':size,'count':count},meta['retention_samples'])
staged('buffer',[meta['public_buffer_count'],meta['public_buffer_rounds'],meta['public_buffer_period']],{'count':meta['public_buffer_count'],'rounds':meta['public_buffer_rounds'],'period':meta['public_buffer_period']},meta['public_buffer_samples'])
meta.update(status='measurements complete',runtime_invocations=len(rows),cpu8_released=True)
(p/'metadata.json').write_text(json.dumps(meta,indent=2)+'\n')
print('COMPLETE',len(rows),flush=True)
