from pathlib import Path
import csv,hashlib,json,os,resource,subprocess,sys,threading,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent
meta=json.loads((p/'metadata.json').read_text())
assert hashlib.sha256((p/'luajit').read_bytes()).hexdigest()==meta['binary_sha256']
mode,workers,pool,waves,batches,count,sentinel,retain,checkpoint=sys.argv[1:]
stem='-'.join(sys.argv[1:])
out=p/stem;out.mkdir(exist_ok=False)
cmd=['taskset','-c',meta['cpus'],str(p/'luajit'),'-j'+mode,str(p/'churn.lua'),workers,pool,waves,batches,count,sentinel,retain,checkpoint]
result=dict(command=cmd,binary_sha256=meta['binary_sha256'],script_sha256=hashlib.sha256((p/'churn.lua').read_bytes()).hexdigest(),started=time.time(),max_rss_kib=0,max_threads=0,stop_reason=None)
(out/'command.json').write_text(json.dumps(result,indent=2)+'\n')
fields=['VmSize','VmRSS','VmHWM','RssAnon','RssFile','VmSwap','Threads']
def status(pid):
 d={}
 for line in Path(f'/proc/{pid}/status').read_text().splitlines():
  if ':' in line:
   k,v=line.split(':',1)
   if k in fields:d[k]=int(v.split()[0])
 return d
finished=threading.Event()
start=time.monotonic()
with (out/'stderr').open('w') as err,(out/'stdout').open('w') as raw,(out/'rows.csv').open('w') as csvout:
 proc=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,text=True,bufsize=1)
 def watchdog():
  while not finished.wait(.05):
   try:s=status(proc.pid)
   except (FileNotFoundError,ProcessLookupError):break
   result['max_rss_kib']=max(result['max_rss_kib'],s.get('VmRSS',0))
   result['max_threads']=max(result['max_threads'],s.get('Threads',0))
   if s.get('VmRSS',0)>meta['rss_limit_kib']:result['stop_reason']='rss_limit'
   if time.monotonic()-start>meta['timeout_seconds']:result['stop_reason']='time_limit'
   if result['stop_reason']:
    proc.kill();break
 monitor=threading.Thread(target=watchdog,daemon=True);monitor.start()
 header=proc.stdout.readline();raw.write(header);raw.flush()
 columns=next(csv.reader([header])) if header else []
 writer=csv.DictWriter(csvout,fieldnames=columns+['proc_'+k for k in fields]+['controller_seconds'])
 writer.writeheader()
 for line in proc.stdout:
  raw.write(line);raw.flush()
  vals=next(csv.reader([line]));assert len(vals)==len(columns),(vals,columns)
  row=dict(zip(columns,vals));s=status(proc.pid)
  row.update({'proc_'+k:s.get(k,'') for k in fields})
  row['controller_seconds']=time.monotonic()-start
  writer.writerow(row);csvout.flush()
  result['max_rss_kib']=max(result['max_rss_kib'],s.get('VmRSS',0))
  result['max_threads']=max(result['max_threads'],s.get('Threads',0))
  proc.stdin.write('go\n');proc.stdin.flush()
 rc=proc.wait(timeout=10);finished.set();monitor.join(timeout=2)
result.update(exit=rc,seconds=time.monotonic()-start)
(out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2),flush=True)
sys.exit(rc!=0)
