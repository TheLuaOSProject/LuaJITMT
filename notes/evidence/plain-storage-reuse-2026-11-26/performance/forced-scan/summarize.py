from pathlib import Path
import csv,json,collections,statistics,hashlib,shutil
p=Path(__file__).resolve().parent
rows=list(csv.DictReader((p/'results.csv').open()));groups=collections.defaultdict(list)
for r in rows:groups[(r['variant'],r['sample'],r['policy'])].append(r)
fields=['variant','sample','policy','ops','work_cpu_seconds','explicit_gc_cpu_seconds','mean_ns_per_op','round_ns_min','round_ns_max','rss_start_kib','rss_end_kib','rss_after_drop_gc_kib','vm_start_kib','vm_end_kib','lua_start_bytes','lua_end_bytes','lua_after_drop_gc_bytes','cycle_starts_at_warm','cycle_starts_at_end','cycle_starts_after_drop_gc']
summary=[]
for (variant,sample,policy),rs in groups.items():
 w=[r for r in rs if r['stage']=='work'];a=[r for r in rs if r['stage']=='after_gc'];start=next(r for r in rs if r['stage']=='warm');end=w[-1];drop=next(r for r in rs if r['stage']=='quiescent_gc_5');cpus=[float(r['work_cpu']) for r in w];ops=int(end['total_ops'])
 summary.append([variant,int(sample),policy,ops,sum(cpus),sum(float(r['gc_cpu']) for r in a),sum(cpus)*1e9/ops,min(cpus)*1e9/20000,max(cpus)*1e9/20000,int(start['proc_VmRSS']),int(end['proc_VmRSS']),int(drop['proc_VmRSS']),int(start['proc_VmSize']),int(end['proc_VmSize']),int(start['lua_bytes']),int(end['lua_bytes']),int(drop['lua_bytes']),int(start['gc_cycle_starts']),int(end['gc_cycle_starts']),int(drop['gc_cycle_starts'])])
with (p/'summary.csv').open('w') as out:w=csv.writer(out);w.writerow(fields);w.writerows(summary)
records=[dict(zip(fields,r)) for r in summary]
lines=['Paired immutable old/new, JIT enabled, CPU 8.40 rounds x20,000 cycles,1KiB reset/put/get.','Each policy includes3 alternating paired samples; work CPU excludes diagnostics and explicitGC.','policy old_ns new_ns new/old paired_min..max old/new_RSS_end_KiB old/new_RSS_after_drop5GC_KiB old/new_explicitGC_CPU_s']
paired=[]
for policy in ['auto','full_each_round','full_every_5_rounds']:
 a={r['sample']:r for r in records if r['policy']==policy and r['variant']=='baseline'};b={r['sample']:r for r in records if r['policy']==policy and r['variant']=='candidate'}
 ratios=[]
 for sample in a:
  ratio=b[sample]['work_cpu_seconds']/a[sample]['work_cpu_seconds'];ratios.append(ratio);paired.append([policy,sample,a[sample]['mean_ns_per_op'],b[sample]['mean_ns_per_op'],ratio,a[sample]['rss_end_kib'],b[sample]['rss_end_kib']])
 med=lambda rs,k:statistics.median(r[k] for r in rs.values());an,bn=med(a,'mean_ns_per_op'),med(b,'mean_ns_per_op')
 lines.append(f"{policy} {an:.3f} {bn:.3f} {bn/an:.4f} {min(ratios):.4f}..{max(ratios):.4f} {med(a,'rss_end_kib')}/{med(b,'rss_end_kib')} {med(a,'rss_after_drop_gc_kib')}/{med(b,'rss_after_drop_gc_kib')} {med(a,'explicit_gc_cpu_seconds'):.6f}/{med(b,'explicit_gc_cpu_seconds'):.6f}")
(p/'summary.txt').write_text('\n'.join(lines)+'\n')
with (p/'paired.csv').open('w') as out:w=csv.writer(out);w.writerow(['policy','sample','baseline_ns_per_op','candidate_ns_per_op','ratio','baseline_end_rss_kib','candidate_end_rss_kib']);w.writerows(paired)
with (p/'round-medians.csv').open('w') as out:
 w=csv.writer(out);w.writerow(['variant','policy','round','total_ops','work_ns_per_op','rss_kib','vm_kib','lua_bytes','phase','cycle_starts'])
 for variant in ['baseline','candidate']:
  for policy in ['auto','full_each_round','full_every_5_rounds']:
   for n in range(1,41):
    rs=[r for r in rows if r['variant']==variant and r['policy']==policy and r['stage']=='work' and int(r['round'])==n];m=lambda k:statistics.median(float(r[k]) for r in rs)
    w.writerow([variant,policy,n,n*20000,m('work_cpu')*1e9/20000,m('proc_VmRSS'),m('proc_VmSize'),m('lua_bytes'),m('gc_phase'),m('gc_cycle_starts')])
selected=[]
for variant in ['baseline','candidate']:
 f=next(p.glob('census-*-'+variant+'-bins.jsonl'));allbins=[json.loads(x) for x in f.read_text().splitlines()]
 for stage,round_ in [('warm',0),('work',1),('work',5),('work',10),('explicit_free',10),('quiescent_gc_1',10),('quiescent_gc_5',10)]:
  a=next(r for r in allbins if r['type']=='arenas' and r['kind']==1 and r['list']=='owned' and r['stage']==stage and r['round']==round_)
  bs=[r for r in allbins if r['type']=='bin' and r['kind']==1 and r['stage']==stage and r['round']==round_]
  b=next((r for r in bs if r['bin']==31),{'nodes':0,'fit259':0,'histogram':[]})
  selected.append({'variant':variant,'stage':stage,'round':round_,'operations':round_*20000,'plain_owned_arenas':a['arenas'],'block_starts':a['block_starts'],'late_starts':a['late_starts'],'late_extent_bytes':a['late_extent_cells']*16,'bin31_nodes':b['nodes'],'bin31_fit259':b['fit259'],'bin31_113cell_nodes':dict(b['histogram']).get(113,0),'remote_closed':a['remote_closed'],'remote_sealed':a['remote_sealed'],'remote_pending':a['remote_pending'],'remote_actors':a['remote_actors']})
(p/'census-summary.json').write_text(json.dumps(selected,indent=2)+'\n')
with (p/'census-summary.csv').open('w') as out:w=csv.DictWriter(out,fieldnames=selected[0]);w.writeheader();w.writerows(selected)
meta=json.loads((p/'metadata.json').read_text());meta.update(result_rows=len(rows),work_rounds=sum(r['stage']=='work' for r in rows),census_rows=sum(1 for _ in csv.DictReader((p/'census-results.csv').open())),all_semantic_assertions='passed in all18policy processes and2census processes',scope='single-mutator; no shared source edits; CPU8 released',lua_workload_sha256=hashlib.sha256((p/'growth.lua').read_bytes()).hexdigest(),original_lua_workload_sha256=hashlib.sha256(Path('/tmp/lj-buffer-churn-growth-20261126-fu6iac_1/growth.lua').read_bytes()).hexdigest())
assert meta['lua_workload_sha256']==meta['original_lua_workload_sha256']
(p/'metadata.json').write_text(json.dumps(meta,indent=2)+'\n')
print('\n'.join(lines))
print('rows',len(rows),'work_rounds',meta['work_rounds'],'census_rows',meta['census_rows'])
print('candidate round ns range',min(float(r['work_cpu'])*1e9/20000 for r in rows if r['variant']=='candidate' and r['stage']=='work'),max(float(r['work_cpu'])*1e9/20000 for r in rows if r['variant']=='candidate' and r['stage']=='work'))
