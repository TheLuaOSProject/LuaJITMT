from pathlib import Path
import json,hashlib,datetime,subprocess
P=Path(__file__).resolve().parent
def sha(path):
 h=hashlib.sha256()
 with path.open('rb') as f:
  for b in iter(lambda:f.read(1048576),b''):h.update(b)
 return h.hexdigest()
def read(name):return json.loads((P/name).read_text())
m=read('source-manifest.json')
for v in m['variants']:
 for e in m['tracked_source_test_files']:assert sha(P/v/e['path'])==e['sha256'],(v,e['path'])
files=['strict-fixtures.json','asan-fixtures.json','normal-lua.json','strict-lua.json','asan-lua.json']
counts={};commands=0;tests=0
for name in files:
 rows=read(name);assert all(r['exit']==0 and r['status']=='complete' for r in rows)
 compile_count=sum(r['name'].startswith('compile-') for r in rows)
 counts[name]={'commands':len(rows),'compilations':compile_count,'test_executions':len(rows)-compile_count}
 commands+=len(rows);tests+=len(rows)-compile_count
for v in ['normal','strict','asan']:
 assert read('build-'+v+'.json')['exit']==0;commands+=1
 rows=read(v+'-lua.json')
 assert next(r for r in rows if r['name']=='stock-joff')['stdout'].strip()=='387 passed'
 assert next(r for r in rows if r['name']=='stock-jon')['stdout'].strip()=='509 passed'
 for r in read('asan-lua.json'):
  assert r['environment']['ASAN_OPTIONS']=='detect_leaks=1:abort_on_error=1'
 for r in read('asan-fixtures.json'):
  assert r['ASAN_OPTIONS']=='detect_leaks=1:abort_on_error=1'
out={'frozen_at_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'base':m['base'],'apply_results':read('apply-results.json'),'source_manifest_sha256':sha(P/'source-manifest.json'),'source_files_per_variant':len(m['tracked_source_test_files']),'all_source_hashes_rechecked':True,'binaries':read('build-binary-manifest.json'),'commands':commands,'test_executions':tests,'counts':counts,'failures':0,'timeouts':0,'sanitizer_reports':0,'target_only_asan_proof':'asan-instrumentation.json','runtime_ASAN_OPTIONS':'detect_leaks=1:abort_on_error=1','sanitizer_suppressions':[],'performance_reruns':False,'shared_source_comparison':read('shared-source-comparison.json'),'known_excluded_failure':'Shared cdata JIT hammer line80 remains known to fail; active-MT recorder refusal retained; not counted as passing.','artifacts':[]}
for f in sorted(P.iterdir()):
 if f.is_file() and f.name not in ['final-validation.json','final-validation.sha256']:
  out['artifacts'].append({'path':f.name,'bytes':f.stat().st_size,'sha256':sha(f)})
(P/'final-validation.json').write_text(json.dumps(out,indent=2)+'\n')
(P/'final-validation.sha256').write_text(sha(P/'final-validation.json')+'  final-validation.json\n')
print('commands',commands,'test executions',tests,'artifacts',len(out['artifacts']))
print((P/'final-validation.sha256').read_text(),end='')
