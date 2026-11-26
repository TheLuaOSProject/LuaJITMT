from pathlib import Path
import csv,hashlib,json,statistics
p=Path(__file__).resolve().parent
summaries=[];waves=[]
for f in sorted(p.glob('*/result.json')):
 result=json.loads(f.read_text());rows=list(csv.DictReader((f.parent/'rows.csv').open()))
 if not rows:continue
 first,last=rows[0],rows[-1]
 def selected(stage):
  rs=[r for r in rows if r['stage']==stage]
  return rs[-1] if rs else None
 def rss(stage):
  r=selected(stage);return int(r['proc_VmRSS']) if r else None
 summary=dict(run=f.parent.name,scenario=first['scenario'],jit=first['jit'],workers=int(first['workers']),gc_workers=int(first['gc_workers']),waves=int(last['wave']),exit=result['exit'],stop_reason=result['stop_reason'],seconds=result['seconds'],max_rss_kib=result['max_rss_kib'],max_threads=result['max_threads'],initial_rss_kib=int(first['proc_VmRSS']),retained_join_rss_kib=rss('joined_retained'),dropped_join_rss_kib=rss('joined_dropped'),after_auto_churn_rss_kib=rss('main_automatic_churn'),final_full_gc_rss_kib=rss('final_full_gc_3'),after_stop_full_gc_rss_kib=rss('after_stop_full_gc_3'),initial_lua_bytes=int(first['lua_bytes']),final_lua_bytes=int(last['lua_bytes']),initial_gc_cycles=int(first['gc_cycle_starts']),final_gc_cycles=int(last['gc_cycle_starts']),total_ops=int(last['total_ops']),rows=len(rows),runtime_stderr_bytes=(f.parent/'stderr').stat().st_size)
 summaries.append(summary)
 for r in rows:
  if r['stage'] in ['joined_retained','joined_dropped','retained_handles_dropped','sentinel_full_gc_3','final_full_gc_3','after_stop_full_gc_3']:
   waves.append(dict(run=f.parent.name,scenario=r['scenario'],jit=r['jit'],workers=int(r['workers']),gc_workers=int(r['gc_workers']),wave=int(r['wave']),stage=r['stage'],retained_handles=int(r['retained_handles']),live_workers=int(r['live_workers']),rss_kib=int(r['proc_VmRSS']),vm_kib=int(r['proc_VmSize']),lua_bytes=int(r['lua_bytes']),phase=int(r['gc_phase']),gc_cycles=int(r['gc_cycle_starts']),total_ops=int(r['total_ops']),os_threads=int(r['proc_Threads'])))
(p/'summary.json').write_text(json.dumps(summaries,indent=2)+'\n')
(p/'wave-summary.json').write_text(json.dumps(waves,indent=2)+'\n')
for name,rs in [('summary.csv',summaries),('wave-summary.csv',waves)]:
 if rs:
  with (p/name).open('w') as out:
   w=csv.DictWriter(out,fieldnames=list(rs[0]));w.writeheader();w.writerows(rs)
for r in summaries:
 print(r['run'],'exit',r['exit'],'rss',r['initial_rss_kib'],r['dropped_join_rss_kib'],r['after_auto_churn_rss_kib'],r['final_full_gc_rss_kib'],r['after_stop_full_gc_rss_kib'],'cycles',r['initial_gc_cycles'],r['final_gc_cycles'])
