from pathlib import Path
import os,subprocess,signal,time,json,hashlib,statistics,math
p=Path(__file__).parent;meta=json.loads((p/'metadata.json').read_text());results=[]
for case in ['tab_store_existing','tab_hash_read','tab_read_existing','tab_array']:
 for pair in range(1,8):
  for name in (['before','after'] if pair%2 else ['after','before']):
   root=Path(meta['roots'][name]);label=f'{case}-{pair:02}-{name}'
   env=os.environ.copy();env['BENCH_SCALE']='0.005';env.pop('BENCH_GC_MODE',None);env['LUA_PATH']=str(root/'src/?.lua')+';;'
   cmd=['taskset','-c','30','stdbuf','-oL','-eL',str(root/'src/luajit'),'-joff',str(Path(meta['roots']['after'])/'plan/aux/bench/bench.lua'),case]
   start=time.monotonic()
   with (p/(label+'.stdout')).open('w') as out,(p/(label+'.stderr')).open('w') as err:
    proc=subprocess.Popen(cmd,env=env,stdout=out,stderr=err,start_new_session=True)
    try:proc.wait(timeout=30);status='complete'
    except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait();status='timeout'
   values=[line.split() for line in (p/(label+'.stdout')).read_text().splitlines() if line.startswith(case+' ')]
   record={'case':case,'pair':pair,'name':name,'command':cmd,'environment':{'BENCH_SCALE':'0.005','BENCH_GC_MODE':None,'LUA_PATH':env['LUA_PATH']},'status':status,'exit':proc.returncode,'seconds':time.monotonic()-start,'limit_seconds':30,'ns_per_op':float(values[0][2]) if len(values)==1 else None}
   results.append(record);(p/'runs.json').write_text(json.dumps(results,indent=2)+'\n')
   assert status=='complete' and proc.returncode==0 and record['ns_per_op'] is not None,record
 print(case,'complete',flush=True)
summary={}
for case in ['tab_store_existing','tab_hash_read','tab_read_existing','tab_array']:
 rows=[r for r in results if r['case']==case]
 vals={name:[r['ns_per_op'] for r in rows if r['name']==name] for name in ['before','after']}
 ratios=[next(r['ns_per_op'] for r in rows if r['pair']==i and r['name']=='after')/next(r['ns_per_op'] for r in rows if r['pair']==i and r['name']=='before') for i in range(1,8)]
 summary[case]={'median_ns_per_op':{n:statistics.median(v) for n,v in vals.items()},'paired_geomean_after_over_before':math.exp(statistics.mean(map(math.log,ratios))),'pairs':7}
(p/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
assert meta['binary_sha256']=={n:hashlib.sha256((Path(r)/'src/luajit').read_bytes()).hexdigest() for n,r in meta['roots'].items()}
print(json.dumps(summary),flush=True)
