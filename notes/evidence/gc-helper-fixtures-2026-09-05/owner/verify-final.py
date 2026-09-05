from pathlib import Path
import hashlib, json

p = Path(__file__).resolve().parent
sha = lambda f: hashlib.sha256(Path(f).read_bytes()).hexdigest()
checked = {}
def verify(f, expected):
    f = Path(f)
    value = checked.get(str(f))
    if value is None:
        value = checked[str(f)] = sha(f)
    assert value == expected, (str(f), expected, value)

manifest = json.loads((p/'artifact-manifest.json').read_text())
for name, expected in manifest['artifacts'].items():
    verify(p/name, expected)
assert len(manifest['artifacts']) == manifest['count']
for f in sorted(p.glob('*/*results.json')):
    data = json.loads(f.read_text())
    for row in data if isinstance(data, list) else [data]:
        for field in ['inputs', 'transitive_dependencies']:
            for name, expected in row.get(field, {}).items():
                path = Path(name)
                verify(path if path.is_absolute() else p/path, expected)

original = json.loads((p/'runtime-source-identity.json').read_text())
source_checks = 0
base843 = Path('/tmp/lj-gc-auto-control-root-20260905-au933s3w')
for name, values in original['sources'].items():
    for variant in ['candidate', 'strict', 'asan']:
        verify(base843/variant/name, values[variant+'_sha256'])
        source_checks += 1
focused = json.loads((p/'focused-input-identity.json').read_text())
base793 = Path('/tmp/lj-clib-cdata-combined-20260905-bxrxos7h')
for name, values in focused['sources'].items():
    verify(p/'releasehelpers-v2'/name, values['releasehelpers_sha256'])
    for variant in ['strict', 'asan']:
        verify(base793/variant/name, values['combined_'+variant+'_sha256'])
    source_checks += 3
for binaries in focused['binaries'].values():
    for name, expected in binaries.items():
        verify(name, expected)
for f in [p/'runtime-builds.json', p/'releasehelpers-v2-build.json']:
    data = json.loads(f.read_text())
    if f.name == 'releasehelpers-v2-build.json':
        for name, expected in data['binaries'].items():
            verify(p/'releasehelpers-v2'/name, expected)

summary = json.loads((p/'validation-summary.json').read_text())
assert summary['final_runtime'] == {'pass': 10}
assert summary['original_runtime'] == {'failure': 6}
print(json.dumps({'artifact_manifest_sha256': sha(p/'artifact-manifest.json'),
                  'artifact_count': manifest['count'], 'source_checks': source_checks,
                  'unique_artifact_input_dependency_files': len(checked),
                  'final_runtime': summary['final_runtime'],
                  'original_runtime': summary['original_runtime']}, indent=2))
