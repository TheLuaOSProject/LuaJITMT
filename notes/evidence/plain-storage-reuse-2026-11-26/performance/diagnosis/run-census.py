from pathlib import Path
import csv,subprocess,time,json,os
p=Path(__file__).resolve().parent
metadata=json.loads((p/'metadata.json').read_text());metadata.update(rounds=10,samples=1,policies={'auto':0})
status_fields=['VmSize','VmRSS','VmHWM','VmData','RssAnon','RssFile','VmSwap','Threads']
with (p/'census-results.csv').open('w') as out:
 writer=None
 for sample in range(1,metadata['samples']+1):
  policies=list(metadata['policies'].items())
  if sample%2==0:policies.reverse()
  for policy,period in policies:
   stem=f'census-{sample}-{policy}';cmd=['taskset','-c',str(metadata['cpu']),str(p/'census-luajit'),str(p/'growth-census.lua'),policy,str(sample),str(metadata['count']),str(metadata['rounds']),str(period)]
   start=time.monotonic();print('RUN',stem,flush=True)
   with (p/(stem+'.stderr')).open('w') as err,(p/(stem+'.stdout')).open('w') as raw:
    env=os.environ.copy();env['CENSUS_OUT']=str(p/(stem+'-bins.jsonl'));proc=subprocess.Popen(cmd,text=True,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,bufsize=1,env=env)
    header=proc.stdout.readline();raw.write(header);cols=next(csv.reader([header]))
    if cols[:3]!=['policy','sample','stage']:raise SystemExit('bad header '+stem)
    if writer is None:
     writer=csv.DictWriter(out,fieldnames=cols+['proc_'+k for k in status_fields]+['controller_elapsed']);writer.writeheader()
    for line in proc.stdout:
     raw.write(line);raw.flush()
     if line.startswith('@closed,'):
      status={}
      for field in Path(f'/proc/{proc.pid}/status').read_text().splitlines():
       if ':' in field:
        k,v=field.split(':',1)
        if k in status_fields:status[k]=int(v.split()[0])
      status.update(cpu_seconds=float(line.split(',')[1]),wall_seconds=float(line.split(',')[2]));(p/'after-lua-close.json').write_text(json.dumps(status,indent=2)+'\n');proc.stdin.write('go\n');proc.stdin.flush();continue
     vals=next(csv.reader([line]));assert len(vals)==len(cols),(stem,line)
     row=dict(zip(cols,vals));status={}
     for field in Path(f'/proc/{proc.pid}/status').read_text().splitlines():
      if ':' in field:
       k,v=field.split(':',1)
       if k in status_fields:status[k]=int(v.split()[0])
     for k in status_fields:row['proc_'+k]=status.get(k,'')
     row['controller_elapsed']=time.monotonic()-start;writer.writerow(row);out.flush()
     proc.stdin.write('go\n');proc.stdin.flush()
    rc=proc.wait(timeout=30)
    if rc:raise SystemExit(f'{stem} failed {rc}')
   print('DONE',stem,round(time.monotonic()-start,3),flush=True)
print('complete',flush=True)
