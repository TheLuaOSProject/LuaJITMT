from pathlib import Path
import hashlib,json
p=Path(__file__).resolve().parent
sha=lambda f:hashlib.sha256(Path(f).read_bytes()).hexdigest()
manifest=json.loads((p/'artifact-manifest.json').read_text())
checked={}
def check(f, expected):
    f=Path(f)
    assert sha(f)==expected,str(f)
    checked[str(f)]=expected
for name,digest in manifest['files'].items(): check(p/name,digest)
identity=json.loads((p/'source-identity.json').read_text())
for variant in ['candidate','optimized','asan']:
    for name, values in identity['sources'].items(): check(p/variant/name,values['candidate_sha256'])
for f in (p/'validation').glob('*/compile.json'):
    obj=json.loads(f.read_text())
    for name,digest in obj['inputs'].items(): check(name,digest)
    for name,digest in obj.get('transitive_dependencies',{}).items(): check(name,digest)
    if obj['exit']==0: check(f.parent/'fixture',obj['binary_sha256'])
for f in (p/'validation').glob('*/results.json'):
    for row in json.loads(f.read_text()):
        for name,digest in row['inputs'].items(): check(name,digest)
for row in json.loads((p/'broad-inputs.json').read_text())['files'].items(): check(row[0],row[1]['sha256'])
summary=json.loads((p/'validation-summary.json').read_text())
assert summary['final_runtime_passes']==123
result={'manifest_count':len(manifest['files']),'manifest_sha256':sha(p/'artifact-manifest.json'),
        'unique_files_verified':len(checked),'source_count_per_build':224,
        'final_runtime_passes':123,'all_runtime_processes':summary['all_recorded_runtime_processes']}
print(json.dumps(result,indent=2))
if not (p/'final-verification.json').exists():
    (p/'final-verification.json').write_text(json.dumps(result,indent=2)+'\n')
