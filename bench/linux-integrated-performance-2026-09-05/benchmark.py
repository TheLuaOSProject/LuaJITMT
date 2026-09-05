from pathlib import Path
import subprocess,json,time,os,signal,hashlib,math,csv
p=Path(__file__).parent;out=p/'performance';out.mkdir(exist_ok=True)
roots={'stock':Path('/tmp/lj-runtime-performance-review-2026-09-04/stock'),'fork':Path('/tmp/lj-linux-integrated-stability-20260905-st7b2_hh/normal')}
harness=roots['fork']/'plan/aux/bench/bench.lua'
rows_expected=['arith_loop','fib30','tab_hash_write','tab_store_existing','tab_insert_newkey','tab_hash_read','tab_read_existing','tab_array','alloc_tables','string_intern','closures_upval','upval_hot','ffi_struct','coroutine_switch','sbuf_format']
meta={'runtime_metadata':json.loads((p/'metadata.json').read_text()),'build':json.loads((p/'build-normal.json').read_text()),'harness_sha256':hashlib.sha256(harness.read_bytes()).hexdigest(),'stock_ref':'b925b3e3fc6771171602323b45fbe9fb8fc90369','binary_sha256':{name:hashlib.sha256((root/'src/luajit').read_bytes()).hexdigest() for name,root in roots.items()},'protocol':'Fresh stock JIT, fork JIT, stock interpreter, fork interpreter; BENCH_SCALE=1, GC mode unset, unmodified full harness, 180s per process. Each row is minimum of five in-process rounds, not independent process repetitions. CPU 30; other functional work on CPUs 0-15; no full host/frequency isolation.'}
(out/'metadata.json').write_text(json.dumps(meta,indent=2)+'\n');results=[]
for name,mode in [('stock','-jon'),('fork','-jon'),('stock','-joff'),('fork','-joff')]:
 root=roots[name];label=name+'-'+mode[1:]
 env=os.environ.copy();env['BENCH_SCALE']='1';env.pop('BENCH_GC_MODE',None);env.pop('BENCH_FILTER',None);env.pop('BENCH_ONLY',None);env['LUA_PATH']=str(root/'src/?.lua')+';;'
 cmd=['taskset','-c','30','stdbuf','-oL','-eL',str(root/'src/luajit'),mode,str(harness)]
 start=time.monotonic()
 with (out/(label+'.stdout')).open('w') as stdout,(out/(label+'.stderr')).open('w') as stderr:
  proc=subprocess.Popen(cmd,stdout=stdout,stderr=stderr,env=env,start_new_session=True)
  try:proc.wait(timeout=180);status='complete'
  except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait();status='timeout'
 data={}
 for line in (out/(label+'.stdout')).read_text().splitlines():
  v=line.split()
  if len(v)==3 and v[0] in rows_expected:data[v[0]]={'total_s':float(v[1]),'ns_per_op':float(v[2])}
 r={'name':name,'mode':mode,'command':cmd,'environment':{'BENCH_SCALE':'1','BENCH_GC_MODE':None,'LUA_PATH':env['LUA_PATH']},'exit':proc.returncode,'status':status,'seconds':time.monotonic()-start,'limit_seconds':180,'rows':data,'missing_rows':[x for x in rows_expected if x not in data]};results.append(r);(out/'runs.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps({k:v for k,v in r.items() if k not in ['rows','command','environment']}),flush=True)
summary={}
for mode in ['-jon','-joff']:
 stock=next(r for r in results if r['name']=='stock' and r['mode']==mode);fork=next(r for r in results if r['name']=='fork' and r['mode']==mode)
 complete=all(r['exit']==0 and not r['missing_rows'] for r in [stock,fork])
 summary[mode]={'complete':complete,'geometric_mean_fork_over_stock':math.exp(sum(math.log(fork['rows'][n]['ns_per_op']/stock['rows'][n]['ns_per_op']) for n in rows_expected)/len(rows_expected)) if complete else None,'reported_fork_rows':len(fork['rows'])}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
with (out/'comparison.csv').open('w',newline='') as f:
 w=csv.writer(f,lineterminator='\n');w.writerow(['mode','benchmark','stock_ns_per_op','fork_ns_per_op','fork_over_stock'])
 for mode in ['-jon','-joff']:
  stock=next(r for r in results if r['name']=='stock' and r['mode']==mode);fork=next(r for r in results if r['name']=='fork' and r['mode']==mode)
  for n in rows_expected:
   s=stock['rows'].get(n,{}).get('ns_per_op');a=fork['rows'].get(n,{}).get('ns_per_op');w.writerow([mode,n,s,a,a/s if s is not None and a is not None else None])
assert meta['binary_sha256']=={name:hashlib.sha256((root/'src/luajit').read_bytes()).hexdigest() for name,root in roots.items()}
print(json.dumps(summary),flush=True)
