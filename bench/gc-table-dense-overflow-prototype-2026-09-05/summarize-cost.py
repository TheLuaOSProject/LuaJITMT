from pathlib import Path
from statistics import median
import json
p=Path(__file__).parent;a=json.loads((p/'cost-results.json').read_text());summary={'samples':len(a),'complete':all(r['exit']==0 for r in a),'barrier':[],'plan':[],'memory':[]}
def pct(a,b):return (b/a-1)*100
for key in ['ordinary_base_to_dense','dense_ordinary_to_promoted']:
 left=[r for r in a if r['kind']=='barrier' and r['variant']==('base-normal' if key=='ordinary_base_to_dense' else 'normal') and r['name']=='ordinary']
 right=[r for r in a if r['kind']=='barrier' and r['variant']=='normal' and r['name']==('ordinary' if key=='ordinary_base_to_dense' else 'promoted')]
 l=[next(d['ns_per_op'] for d in r['data'] if d['type']=='barrier') for r in left]
 r=[next(d['ns_per_op'] for d in rr['data'] if d['type']=='barrier') for rr in right]
 ratios=[pct(x,y) for x,y in zip(l,r)]
 summary['barrier'].append({'comparison':key,'left_median_ns':median(l),'right_median_ns':median(r),'paired_percent_median':median(ratios),'paired_percent_min':min(ratios),'paired_percent_max':max(ratios),'left':l,'right':r,'ratios':ratios})
for name in ['alloc_tables','tab_insert_newkey','closures_upval']:
 for mode in ['joff','jon']:
  l=[r['ns_per_op'] for r in a if r['kind']=='plan' and r['name']==name and r['mode']==mode and r['variant']=='base-normal']
  r=[r['ns_per_op'] for r in a if r['kind']=='plan' and r['name']==name and r['mode']==mode and r['variant']=='normal']
  ratios=[pct(x,y) for x,y in zip(l,r)]
  summary['plan'].append({'name':name,'mode':mode,'base_median_ns':median(l),'dense_median_ns':median(r),'paired_percent_median':median(ratios),'paired_percent_min':min(ratios),'paired_percent_max':max(ratios),'base':l,'dense':r,'ratios':ratios})
for name in ['tables','insertion','closures','promoted_tables']:
 for mode in ['joff','jon']:
  row={'name':name,'mode':mode}
  for variant in ['base-normal','normal']:
   rs=[r for r in a if r['kind']=='memory' and r['name']==name and r['mode']==mode and r['variant']==variant]
   data=[]
   for r in rs:
    d={x['stage']:x for x in r['data'] if x['type']=='memory'}
    w=next(x for x in r['data'] if x['type']=='workload')
    data.append({'cpu_seconds':w['cpu_seconds'],'snapshots':d})
   result={'cpu_seconds_median':median(d['cpu_seconds'] for d in data)}
   for stage in ['retained_collected','promoted_collected','released_collected','after_close']:
    if stage not in data[0]['snapshots']:continue
    snaps=[d['snapshots'][stage] for d in data]
    result[stage]={k:median(x[k] for x in snaps) for k in snaps[0] if k not in ['stage','type']}
    result[stage]['rss_since_open_kb']=median(d['snapshots'][stage]['rss_kb']-d['snapshots']['opened']['rss_kb'] for d in data)
    result[stage]['vsize_since_open_kb']=median(d['snapshots'][stage]['vsize_kb']-d['snapshots']['opened']['vsize_kb'] for d in data)
   if name=='promoted_tables':result['promotion_rss_delta_kb']=median(d['snapshots']['promoted_collected']['rss_kb']-d['snapshots']['retained_collected']['rss_kb'] for d in data)
   row[variant]=result
  summary['memory'].append(row)
(p/'cost-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print('samples',len(a),summary['complete'])
for typ in ['barrier','plan']:
 for r in summary[typ]:print(typ,{k:v for k,v in r.items() if not isinstance(v,list)})
for r in summary['memory']:
 print('memory',r['name'],r['mode'])
 for variant in ['base-normal','normal']:
  d=r[variant];x=d['retained_collected'];y=d['released_collected']
  print(variant,'cpu',d['cpu_seconds_median'],'mapped',x['traversable_mappings'],'sidecar',x['sidecar_requested'],x['sidecar_usable'],'RSS retain/delta/free',x['rss_kb'],x['rss_since_open_kb'],y['rss_kb'],'VmSize',x['vsize_kb'],'mmapcount/bytes',x['malloc_mmaps'],x['malloc_mmap_bytes'],'promotionRSS',d.get('promotion_rss_delta_kb'))
