from pathlib import Path
import difflib, hashlib, json

p = Path(__file__).resolve().parent
base = json.loads((p/'base-inputs.json').read_text())
source = Path(base['base_runtime'])
candidate = p/'candidate'
sha = lambda f: hashlib.sha256(Path(f).read_bytes()).hexdigest()
rows = {}
changed = []
patch = ''
for name, expected in base['sources'].items():
    assert sha(source/name) == expected, name
    value = sha(candidate/name)
    rows[name] = {'base_sha256': expected, 'candidate_sha256': value}
    if value != expected:
        changed.append(name)
        patch += ''.join(difflib.unified_diff(
            (source/name).read_text().splitlines(keepends=True),
            (candidate/name).read_text().splitlines(keepends=True),
            fromfile='a/'+name, tofile='b/'+name))
assert changed == ['src/lj_tab.c', 'src/lj_tab.h'], changed
for name in ['candidate-v1.patch', 'source-identity.json']:
    assert not (p/name).exists(), name
(p/'candidate-v1.patch').write_text(patch)
(p/'source-identity.json').write_text(json.dumps({
    'base_commit': base['base_commit'], 'count': len(rows), 'sources': rows,
    'changed': changed, 'patch_sha256': sha(p/'candidate-v1.patch'),
    'note': 'Only lj_tab.c has production code changes. lj_tab.h adds a helper-only direct probe declaration and hook contract.'
}, indent=2)+'\n')
print('patch', sha(p/'candidate-v1.patch'))
for name in changed:
    print(name, rows[name]['candidate_sha256'])
