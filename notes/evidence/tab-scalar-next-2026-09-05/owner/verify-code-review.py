from pathlib import Path
import hashlib, json

p = Path(__file__).resolve().parent
sha = lambda f: hashlib.sha256(Path(f).read_bytes()).hexdigest()
checked = {}
def check(f, expected):
    f = Path(f)
    if str(f) not in checked:
        checked[str(f)] = sha(f)
    assert checked[str(f)] == expected, (str(f), expected, checked[str(f)])
manifest = json.loads((p/'code-review-manifest.json').read_text())
assert len(manifest['artifacts']) == manifest['count']
for name, expected in manifest['artifacts'].items():
    check(p/name, expected)
identity = json.loads((p/'source-identity.json').read_text())
base = json.loads((p/'base-inputs.json').read_text())
for name, values in identity['sources'].items():
    check(p/'candidate'/name, values['candidate_sha256'])
    check(Path(base['base_runtime'])/name, values['base_sha256'])
for name, expected in base['metadata'].items():
    check(p/'candidate'/name, expected)
for f in [p/'candidate-build.json', p/'idle-compile.json']:
    record = json.loads(f.read_text())
    assert record['exit'] == 0
    for field in ['inputs', 'transitive_dependencies']:
        for name, expected in record.get(field, {}).items():
            check(name, expected)
    for name, expected in record.get('binaries', {}).items():
        check(p/'candidate'/name, expected)
    if 'binary_sha256' in record:
        check(p/'idle-candidate', record['binary_sha256'])
check(base['approved_proof'], base['approved_proof_sha256'])
print(json.dumps({'manifest_count': manifest['count'], 'manifest_sha256': sha(p/'code-review-manifest.json'),
                  'source_count_per_tree': len(identity['sources']), 'unique_files_checked': len(checked),
                  'stage': 'source/build review', 'runtime_passes': 0}, indent=2))
