from pathlib import Path
import hashlib,json,resource,statistics,subprocess,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent
meta=json.loads((p/'metadata.json').read_text())
for variant in ['baseline','candidate']:
 assert hashlib.sha256((p/(variant+'-live-growth')).read_bytes()).hexdigest()==meta['live_growth_binary_sha256'][variant]
rows=[]
for count in meta['live_growth_sizes']:
 for sample in range(1,meta['live_growth_samples']+1):
  for order,variant in enumerate(['baseline','candidate'] if sample%2 else ['candidate','baseline'],1):
   stem=f'live-{count}-{sample}-{order}-{variant}'
   cmd=['taskset','-c',str(meta['cpu']),str(p/(variant+'-live-growth')),str(count)]
   t=time.monotonic()
   r=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
   (p/(stem+'.stdout')).write_text(r.stdout);(p/(stem+'.stderr')).write_text(r.stderr)
   row=dict(variant=variant,count=count,sample=sample,order=order,command=cmd,exit=r.returncode,controller_seconds=time.monotonic()-t,stdout=r.stdout,stderr=r.stderr)
   if r.returncode==0:
    n,seconds,ns=r.stdout.strip().split(',');row.update(allocations=int(n),seconds=float(seconds),ns_per_allocation=float(ns))
   rows.append(row);(p/'live-growth.json').write_text(json.dumps(rows,indent=2)+'\n')
   print(stem,r.returncode,flush=True);assert r.returncode==0,stem
summary=[]
for count in meta['live_growth_sizes']:
 a={r['sample']:r for r in rows if r['variant']=='baseline' and r['count']==count};b={r['sample']:r for r in rows if r['variant']=='candidate' and r['count']==count}
 ratios=[b[k]['seconds']/a[k]['seconds'] for k in a]
 summary.append(dict(arenas_requested=count,baseline_ns=statistics.median(r['ns_per_allocation'] for r in a.values()),candidate_ns=statistics.median(r['ns_per_allocation'] for r in b.values()),paired_median_ratio=statistics.median(ratios),paired_min_ratio=min(ratios),paired_max_ratio=max(ratios),all_ratios=ratios))
(p/'live-growth-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
