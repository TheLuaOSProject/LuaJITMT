from pathlib import Path
from collections import Counter
import json, re
r=Path(__file__).resolve().parent
vs=['candidate3','strict','asan']
files={
 'production_string_matrix':[v+'-results.json' for v in vs],
 'sequential_stop_attachment_native_controls':[v+'-controls-results.json' for v in vs],
 'active_stop_controls':[v+'-controls-v2-results.json' for v in vs],
 'first_attachment_stop_restart_numeric_max':[v+'-boundary-overlaps-results.json' for v in vs],
 'last_detachment_restart':[v+'-last-detach-results.json' for v in ['candidate3','strict']],
 'nested_foreign_throwing_finalizer_controls':[v+'-finalizers-v3-results.json' for v in vs],
 'unchanged_existing_finalizers':[v+'-existing-finalizers-results.json' for v in vs],
 'deterministic_spawn_query_logically_enabled':[v+'-spawn-query-v3-results.json' for v in vs],
}
result={'source':json.loads((r/'candidate3-source.json').read_text()),'groups':{}}
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
normal=[x for g in result['groups'].values() for x in g['cases']]
result['runtime_count']=len(normal)
result['exit_counts']=dict(Counter(str(x['exit_code']) for x in normal))
result['asan_runtime_count']=sum(bool(x.get('ASAN_OPTIONS')) for x in normal)
result['asan_reports']=[x for x in normal if x.get('sanitizer_report')]
result['semantics']='Exit2 preserves worker-two SWEEP completion bound; public-control/finalizer passes do not resolve it.'
(r/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
print({k:v for k,v in result.items() if k not in ['groups','source']})
for name,g in result['groups'].items():print(name,g['runtimes'],g['exit_counts'])
