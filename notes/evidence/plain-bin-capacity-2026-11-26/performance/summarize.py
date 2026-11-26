from pathlib import Path
import csv,json,statistics as s
p=Path(__file__).resolve().parent
rows=json.loads((p/'measurements.json').read_text())
long=json.loads((p/'long-hot-measurements.json').read_text())
isolated=json.loads((p/'long-hot-isolated-measurements.json').read_text())
assert len(rows)==84 and len(long)==10 and len(isolated)==10
assert all(r['exit']==0 and not r['stderr'] for r in rows+long+isolated)
def paired(a,key):
 vals=[]
 for sample in sorted(set(r['sample'] for r in a)):
  d={r['variant']:r for r in a if r['sample']==sample};vals.append(key(d['candidate'])/key(d['baseline']))
 return {'paired_ratios':vals,'median_paired_ratio':s.median(vals),'min_paired_ratio':min(vals),'max_paired_ratio':max(vals)}
def med(a,variant,key):return s.median(key(r) for r in a if r['variant']==variant)
hot=[]
for size in [64,256,4096]:
 a=[r for r in rows if r['kind']=='hot' and r['size']==size]
 hot.append(dict(size=size,operations=500000,baseline_ns=med(a,'baseline',lambda r:r['ns_per_op']),candidate_ns=med(a,'candidate',lambda r:r['ns_per_op']),**paired(a,lambda r:r['ns_per_op'])))
hot.append(dict(size=4096,operations=2000000,classification='potential_sibling_overlap',baseline_ns=med(long,'baseline',lambda r:r['ns_per_op']),candidate_ns=med(long,'candidate',lambda r:r['ns_per_op']),**paired(long,lambda r:r['ns_per_op'])))
hot.append(dict(size=4096,operations=2000000,classification='core_reserved_repeat',baseline_ns=med(isolated,'baseline',lambda r:r['ns_per_op']),candidate_ns=med(isolated,'candidate',lambda r:r['ns_per_op']),**paired(isolated,lambda r:r['ns_per_op'])))
bulk=[]
for count in [1024,4096,9000]:
 for kind in [0,1]:
  a=[r for r in rows if r['kind']=='bulk' and r['count']==count and r['traversable']==kind]
  entry={'count':count,'kind':'traversable' if kind else 'plain','free':paired(a,lambda r:r['free_cpu']),'refill':paired(a,lambda r:r['refill_cpu'])}
  for v in ['baseline','candidate']:
   entry[v]={k:med(a,v,lambda r:r[k]) for k in ['arenas_before','arenas_after','retained_targets','free_cells','expected_cells','reused']}
   entry[v]['free_ns']=med(a,v,lambda r:r['free_cpu']*1e9/count)
   entry[v]['refill_ns']=med(a,v,lambda r:r['refill_cpu']*1e9/count)
  bulk.append(entry)
retention=[]
for size in [4096,16384]:
 a=[r for r in rows if r['kind']=='retention' and r['size']==size]
 entry={'size':size,'count':a[0]['count']}
 for v in ['baseline','candidate']:
  phases={}
  for stage in ['original','shrunk','refilled','all_freed']:
   phases[stage]={k:s.median(float(x[k]) for r in a if r['variant']==v for x in r['phases'] if x['stage']==stage) for k in ['logical_bytes','physical_allocated_bytes','block_starts','arenas','bin_free_bytes','bump_free_bytes','work_cpu','work_wall','proc_VmRSS','proc_VmHWM']}
   phase=phases[stage];phase['usable_arena_bytes']=phase['arenas']*55680
   phase['unaccounted_bytes']=phase['usable_arena_bytes']-phase['physical_allocated_bytes']-phase['bin_free_bytes']-phase['bump_free_bytes']
  entry[v]=phases
  if v=='candidate':assert phases['all_freed']['unaccounted_bytes']==0
 for stage in ['original','shrunk','refilled','all_freed']:
  entry[stage+'_cpu']=paired(a,lambda r:float(next(x for x in r['phases'] if x['stage']==stage)['work_cpu']))
 retention.append(entry)
a=[r for r in rows if r['kind']=='buffer']
def ns(r):return sum(float(x['work_cpu']) for x in r['phases'] if x['stage']=='work')*1e9/(r['count']*r['rounds'])
buffer={'operations_per_process':800000,'samples':3,**paired(a,ns)}
for v in ['baseline','candidate']:
 work=[x for r in a if r['variant']==v for x in r['phases'] if x['stage']=='work']
 buffer[v]={'median_ns':med(a,v,ns),'samples_ns':[ns(r) for r in a if r['variant']==v],'work_rss_min_kib':min(x['proc_VmRSS'] for x in work),'work_rss_max_kib':max(x['proc_VmRSS'] for x in work),'quiescent_rss_kib':[r['phases'][-1]['proc_VmRSS'] for r in a if r['variant']==v],'quiescent_lua_bytes':[int(r['phases'][-1]['lua_bytes']) for r in a if r['variant']==v]}
summary={'hot':hot,'bulk':bulk,'retention':retention,'buffer':buffer,'runtime_invocations':104,'exit_zero':104,'empty_stderr':104}
(p/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
with (p/'hot-summary.csv').open('w') as f:
 keys=['classification']+[k for k in hot[0] if k!='paired_ratios'];w=csv.DictWriter(f,fieldnames=keys);w.writeheader();w.writerows({k:r.get(k,'primary') for k in keys} for r in hot)
with (p/'bulk-summary.csv').open('w') as f:
 keys=['count','kind','baseline_free_ns','candidate_free_ns','median_free_ratio','min_free_ratio','max_free_ratio','baseline_refill_ns','candidate_refill_ns','baseline_reused','candidate_reused','baseline_arenas_after','candidate_arenas_after']
 w=csv.DictWriter(f,fieldnames=keys);w.writeheader()
 for r in bulk:w.writerow(dict(count=r['count'],kind=r['kind'],baseline_free_ns=r['baseline']['free_ns'],candidate_free_ns=r['candidate']['free_ns'],median_free_ratio=r['free']['median_paired_ratio'],min_free_ratio=r['free']['min_paired_ratio'],max_free_ratio=r['free']['max_paired_ratio'],baseline_refill_ns=r['baseline']['refill_ns'],candidate_refill_ns=r['candidate']['refill_ns'],baseline_reused=r['baseline']['reused'],candidate_reused=r['candidate']['reused'],baseline_arenas_after=r['baseline']['arenas_after'],candidate_arenas_after=r['candidate']['arenas_after']))
print('hot',hot)
print('buffer paired ratio',buffer['median_paired_ratio'],buffer['min_paired_ratio'],buffer['max_paired_ratio'])
print('bulk 9000 ratios',[(r['kind'],r['free']) for r in bulk if r['count']==9000])
