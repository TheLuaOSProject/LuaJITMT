from pathlib import Path
import subprocess,os,json,time,statistics,hashlib,signal,csv
out=Path(__file__).parent;harness=out/'base/plan/aux/bench/bench.lua';runs=[];summary=[]
meta={'base_commit':'d680421c4cb50b85437d88255bc89358c5e3a6b1','cpu_requested':32,'cpu_used':31,'affinity_available':sorted(os.sched_getaffinity(0)),'scale':'0.02','normal_builds':True,'pairs':7,'protocol':'Alternating AB/BA, fresh process per sample, unmodified d680 filtered harness; row minimum of five in-process rounds; medians of independent paired ratios. CPU31, no complete host/frequency isolation. Other validation on CPUs0–15 and an independent study may useCPU30.','harness_sha256':hashlib.sha256(harness.read_bytes()).hexdigest(),'binaries':{kind:hashlib.sha256((out/kind/'src/luajit').read_bytes()).hexdigest() for kind in ['base','wide']}}
(out/'timing-metadata.json').write_text(json.dumps(meta,indent=2))
for mode in ['-joff','-jon']:
 for case in ['tab_store_existing','tab_insert_newkey','closures_upval']:
  pairs=[]
  for pair in range(7):
   current={}
   for kind in (['base','wide'] if pair%2==0 else ['wide','base']):
    tree=out/kind;env=os.environ.copy();env['BENCH_SCALE']='0.02';env.pop('BENCH_GC_MODE',None);env['LUA_PATH']=str(tree/'src/?.lua')+';;'
    cmd=['taskset','-c','31',str(tree/'src/luajit'),mode,str(harness),case]
    start=time.monotonic();proc=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=env,start_new_session=True)
    try:stdout,stderr=proc.communicate(timeout=45);status='complete'
    except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);stdout,stderr=proc.communicate();status='timeout'
    values=[line.split() for line in stdout.splitlines() if line.startswith(case+' ')]
    value=float(values[0][2]) if len(values)==1 else None
    row={'mode':mode,'case':case,'pair':pair,'kind':kind,'command':cmd,'environment':{'BENCH_SCALE':'0.02','BENCH_GC_MODE':None,'LUA_PATH':env['LUA_PATH']},'rc':proc.returncode,'status':status,'elapsed':time.monotonic()-start,'ns_per_op':value,'stdout':stdout,'stderr':stderr};runs.append(row);current[kind]=value
    (out/'timing-runs.json').write_text(json.dumps(runs,indent=2))
    if proc.returncode or value is None:raise RuntimeError(row)
   pairs.append(current['wide']/current['base'])
  base=[r['ns_per_op'] for r in runs if r['case']==case and r['mode']==mode and r['kind']=='base'];wide=[r['ns_per_op'] for r in runs if r['case']==case and r['mode']==mode and r['kind']=='wide']
  s={'mode':mode,'case':case,'base_median_ns':statistics.median(base),'wide_median_ns':statistics.median(wide),'median_paired_ratio':statistics.median(pairs),'paired_ratio_min':min(pairs),'paired_ratio_max':max(pairs),'paired_ratios':pairs,'base_samples_ns':base,'wide_samples_ns':wide};summary.append(s);(out/'timing-summary.json').write_text(json.dumps(summary,indent=2));print(s,flush=True)
assert meta['binaries']=={kind:hashlib.sha256((out/kind/'src/luajit').read_bytes()).hexdigest() for kind in ['base','wide']}
with (out/'timing-comparison.csv').open('w',newline='') as f:
 writer=csv.DictWriter(f,fieldnames=['mode','case','base_median_ns','wide_median_ns','median_paired_ratio','paired_ratio_min','paired_ratio_max']);writer.writeheader();writer.writerows({k:r[k] for k in writer.fieldnames} for r in summary)
