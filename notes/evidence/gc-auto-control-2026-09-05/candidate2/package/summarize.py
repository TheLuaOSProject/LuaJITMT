from pathlib import Path
from collections import Counter
import json, hashlib, re
r=Path(__file__).resolve().parent
vs=['candidate2','strict','asan']
files={
 'production_string_matrix':[v+'-results.json' for v in vs],
 'safe_boundaries_and_sequential_controls':[v+'-controls-results.json' for v in vs],
 'active_stop_controls':[v+'-controls-v2-results.json' for v in vs],
 'unchanged_first_attachment_and_numeric_controls':[v+'-boundary-overlaps-results.json' for v in vs],
 'first_attachment_restart_candidate_comparison':['restart-results.json'],
 'last_detachment_restart_comparison':['last-detach-results.json'],
 'numeric_max_candidate_comparison':['numeric-results.json'],
 'nested_foreign_throwing_finalizer_controls':[v+'-finalizers-results.json' for v in ['control',*vs]],
 'initial_finalizer_calibration':['candidate-fin-calibration-results.json'],
 'unchanged_existing_finalizers':[v+'-existing-finalizers-results.json' for v in ['control',*vs]],
 'deterministic_spawn_query_compatibility':[v+'-spawn-query-results.json' for v in ['control',*vs]],
 'existing_safety':['helpers-safety-results.json'],
 'matched_closure_timeout':['controlhelpers-safety-results.json'],
 'repaired_scheduler':['helpers-repaired-scheduler-results.json','helpers-repaired-scheduler-v2-results.json'],
 'diagnostic_debugger':['function-timeout-gdb-results.json'],
}
result={'base':'aee88db569b82216b705408f00295a337a7393fe','patches':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [r/'automatic-control.patch',r/'automatic-control-v2.patch',r/'automatic-control-full.patch']},'groups':{}}
allrows=[]
for group,fs in files.items():
 rows=[]
 for f in fs:
  for row in json.loads((r/f).read_text()):rows.append(dict(row,result_file=f))
 runtime=[x for x in rows if Path(x['argv'][0]).name not in ['cc','clang','gcc','objdump']]
 reports=[]
 for row in runtime:
  data=[]
  for line in (r/row['stdout']).read_text().splitlines():
   if line.startswith('{'):
    try:data.append(json.loads(line))
    except json.JSONDecodeError:pass
  err=(r/row['stderr']).read_text()
  reports.append(dict(case=row.get('case',row.get('name')),exit_code=row.get('exit_code','timeout'),seconds=row['seconds'],stdout=row['stdout'],stderr=row['stderr'],stderr_bytes=len(err.encode()),result_file=row['result_file'],stages=[x for x in data if 'stage' in x],diagnostics=[x for x in data if 'diagnostic' in x],ASAN_OPTIONS=row.get('ASAN_OPTIONS'),sanitizer_report=bool(re.search(r'ERROR: (AddressSanitizer|LeakSanitizer)|SUMMARY: AddressSanitizer',err))))
 result['groups'][group]=dict(result_files=fs,commands=len(rows),nonruntime_failures=[x for x in rows if x not in runtime and x.get('exit_code')!=0],runtimes=len(runtime),exit_counts=dict(Counter(str(x.get('exit_code','timeout')) for x in runtime)),cases=reports)
 allrows+=rows
normal=[x for group,g in result['groups'].items() if group!='diagnostic_debugger' for x in g['cases']]
result['normal_runtime_count']=len(normal)
result['normal_exit_counts']=dict(Counter(str(x['exit_code']) for x in normal))
result['asan_runtime_count']=sum(bool(x.get('ASAN_OPTIONS')) for x in normal)
result['asan_reports']=[x for x in normal if x.get('sanitizer_report')]
result['normal_runtime_semantics']='Includes retained failed candidates, diagnostic oracles, existing fixture failures and incomplete worker bounds; not an all-green score.'
(r/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
print({k:v for k,v in result.items() if k!='groups'})
for name,g in result['groups'].items():print(name,g['runtimes'],g['exit_counts'])
