from pathlib import Path
from collections import Counter
import json,hashlib
r=Path(__file__).resolve().parent
files={
 'production_matrix':[f'{v}-results.json' for v in ['veto','strict','asan']],
 'safe_boundary_and_sequential_controls':[f'{v}-controls-results.json' for v in ['veto','strict','asan']],
 'active_stop_and_last_detach':[f'{v}-controls-v2-results.json' for v in ['veto','strict','asan']],
 'controlled_stop_overlap':['overlap-results.json','veto-overlap-results.json','instrumented-stop-overlap-results.json'],
 'controlled_restart_loss':['restart-overlap-results.json'],
 'restart_after_stop_8192_bound':['stop-overlap-restart-results.json'],
 'restart_after_stop_16384_bound':['stop-overlap-restart-v2-results.json'],
 'existing_safety':['extrahelpers-safety-results.json'],
 'scheduler_isolation':['scheduler-isolation-results.json'],
 'matched_safety':[f'{v}-matched-safety-results.json' for v in ['control','oldcandidate','veto']],
 'diagnostic_debugger':['function-timeout-gdb-results.json'],
}
result={'base':'597b8705208957ade8465416da30976ab9b52195','patches':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [r/'veto-only.patch',r/'veto-full.patch']},'groups':{}}
for group,fs in files.items():
 rows=[]
 for f in fs:
  for row in json.loads((r/f).read_text()):rows.append(dict(row,result_file=f))
 runtime=[x for x in rows if not x.get('name',x.get('case','')).endswith('-compile') and x.get('case')!='compile' and not x.get('name','').endswith('-disassembly')]
 reports=[]
 for row in runtime:
  p=r/row['stdout'];data=[]
  for line in p.read_text().splitlines():
   if line.startswith('{'):data.append(json.loads(line))
  stages=[x for x in data if 'stage' in x];diags=[x for x in data if 'diagnostic' in x]
  reports.append(dict(case=row.get('case',row.get('name')),exit_code=row.get('exit_code','timeout'),seconds=row['seconds'],stdout=row['stdout'],stderr=row['stderr'],stderr_bytes=(r/row['stderr']).stat().st_size,result_file=row['result_file'],stages=stages,diagnostics=diags))
 result['groups'][group]=dict(result_files=fs,commands=len(rows),runtimes=len(runtime),exit_counts=dict(Counter(str(x.get('exit_code','timeout')) for x in runtime)),cases=reports)
normal=[x for name,g in result['groups'].items() if name!='diagnostic_debugger' for x in g['cases']]
result['normal_runtime_count']=len(normal);result['normal_exit_counts']=dict(Counter(str(x['exit_code']) for x in normal));result['asan_production_runtime_count']=sum('asan' in x['case'] or x['result_file'].startswith('asan') for name,g in result['groups'].items() if name not in ['existing_safety','scheduler_isolation','matched_safety','diagnostic_debugger'] for x in g['cases'])
(r/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
print({k:v for k,v in result.items() if k!='groups'})
for name,g in result['groups'].items():print(name,g['runtimes'],g['exit_counts'])
