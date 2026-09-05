from pathlib import Path
import collections,hashlib,json

r=Path(__file__).resolve().parent
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
summary={'production_source':'candidate3','runtime_patch_added':False,'direct_controls':{},
         'observational_timeline':{},'exact_native_frontier':{}}
for variant in ['runtime','debug','asan-candidate']:
    results=json.loads((r/(variant+'-results.json')).read_text())
    rows=[]
    for result in results:
        if result['case']=='compile':continue
        stdout=r/result['stdout'];stderr=r/result['stderr']
        observations=[json.loads(line) for line in stdout.read_text().splitlines()
                      if line.startswith('{') and '"stage"' in line]
        first=observations[0]
        measured=[s for s in observations if s['stage'] in ['settled','automatic_progress_missing']]
        final=measured[-1]
        rows.append({'case':result['case'],'exit_code':result.get('exit_code'),
                     'timeout':result.get('timeout',False),'seconds':result['seconds'],
                     'stage':final['stage'],'phase':final['phase'],
                     'cycle_starts':final['cycles']-first['cycles'],
                     'completed_cycles':final['completed']-first['completed'],
                     'filler_tables':final['filler_tables'],
                     'rounds':final['round'],'stderr_bytes':stderr.stat().st_size,
                     'stdout_sha256':sha(stdout),'stderr_sha256':sha(stderr)})
    summary['direct_controls'][variant]=rows
keys='phase cycle sweep_to_idle cycle_leader sweep_root_scanned sweep_root_done sweep_root_cursor sweep_bridge_ready worker_active worker_runs worker_async_progress worker_parks worker_busy_retries deferred_epoch hs_epoch hs_pending hs_actions grey_top grey_bottom ssb_published ssb_drained ssb_items_published ssb_items_drained recovery_items recovery_failed table_rescan_pending thread_scan_needscan_pending marks_this_round alloc_since_trigger trigger_bytes hard_bytes sweep_owner_runs sweep_owner_arenas smr_readers smr_reclaiming weak_drain_active weak_write_active finalizer_active finalizer_owner_actor finalizer_mpsc finalizer_spawn_latch jit_phase_gate'.split()
for case in ['0-0-2','0-1-2']:
    timeline=[]
    for p in sorted((r/('gdb-v7-'+case)).glob('*.json')):
        d=json.loads(p.read_text())
        row={'input':str(p.relative_to(r)),'sha256':sha(p),'where':d['where'],'tick':d['tick'],
             'gc2':{k:d['gc2'][k] for k in keys},'grey':d['grey'],
             'all_threads_stopped':all(t[3] for t in d['threads']),
             'ssb':{q:[{'address':n['address'],'owner':n['owner'],'n':n['n'],
                        'top_objects':collections.Counter(n['slots']).most_common(4)}
                       for n in d[q]['nodes']] for q in ['ssb_head','ssb_drain']},
             'tgs':[{'tid':t['tid'],'actor_id':t['actor_id'],'cur_L':t['cur_L'],
                     'thread_L':t['thread_L'],'native':t['in_native'],
                     'reader_depths':[t[k] for k in ['strtab_active_depth','strq_active_depth','tab_read_depth']],
                     'private_ssb_count':t['private_ssb_count'],
                     'private_ssb_top':collections.Counter(t['private_ssb_slots']).most_common(4),
                     'pending_root':t['gcroot_pending'],'prepare_epoch':t['alloc']['prepare_epoch'],
                     'arena_counts':{k:len(v['nodes']) for k,v in t['arena_lists'].items()}}
                    for t in d['tgs']]}
        timeline.append(row)
        if d['where']=='native_return' and d['tick']==575:
            summary['exact_native_frontier'][case]={'input':str(p.relative_to(r)),
                'sha256':sha(p),'root_chains':d['root_chains']}
    summary['observational_timeline'][case]=timeline
(r/'diagnosis.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps({'direct_controls':summary['direct_controls'],
                  'diagnosis_sha256':sha(r/'diagnosis.json')}),flush=True)
