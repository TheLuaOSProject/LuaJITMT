from pathlib import Path
import hashlib, json

p = Path(__file__).resolve().parent
sha = lambda f: hashlib.sha256(Path(f).read_bytes()).hexdigest()
checked = {}
def verify(f, expected):
    f = Path(f)
    if str(f) not in checked:
        checked[str(f)] = sha(f)
    assert checked[str(f)] == expected, (str(f), expected, checked[str(f)])

manifest = json.loads((p/'artifact-manifest.json').read_text())
assert manifest['count'] == len(manifest['artifacts'])
for name, expected in manifest['artifacts'].items():
    verify(p/name, expected)
inputs = json.loads((p/'inputs.json').read_text())
tree = Path(inputs['runtime'])
assert len(inputs['sources']) == 224
for name, expected in inputs['sources'].items():
    verify(tree/name, expected)
for f in (p/'review-source').iterdir():
    verify(f, inputs['sources']['src/'+f.name])
verify(inputs['binary'], inputs['binary_sha256'])
verify(tree/'src/libluajit.a', inputs['archive_sha256'])
for name in ['admission-results.json', 'primitive-control-results.json']:
    data = json.loads((p/name).read_text())
    for row in data if isinstance(data, list) else [data]:
        for field in ['inputs', 'transitive_dependencies']:
            for source, expected in row.get(field, {}).items():
                verify(source, expected)
observer = json.loads((p/'admission-results.json').read_text())
assert observer['exit'] == 0 and not observer.get('timeout')
assert 'smr_try result=0 phase=0 smr=1 readers=0 native_gate=0' in observer['stdout']
control = json.loads((p/'primitive-control-results.json').read_text())
assert len(control) == 2 and all(row['exit'] == 0 for row in control)
assert 'ordinary scalar field loop completed before reclaimer release' in control[1]['stdout']
print(json.dumps({'artifact_count': manifest['count'], 'source_count': len(inputs['sources']),
                  'review_source_count': len(list((p/'review-source').iterdir())),
                  'unique_files_checked': len(checked), 'manifest_sha256': sha(p/'artifact-manifest.json'),
                  'stage': 'source proof only', 'unchanged_primitive_control_passes': 1,
                  'candidate_passes': 0}, indent=2))
