from pathlib import Path
import hashlib, json

p = Path(__file__).resolve().parent
sha = lambda f: hashlib.sha256(Path(f).read_bytes()).hexdigest()
identity = json.loads((p/'source-identity.json').read_text())
for name, values in identity['sources'].items():
    assert sha(p/'candidate'/name) == values['candidate_sha256'], name
names = ['base-inputs.json', 'freeze-source.py', 'source-identity.json',
         'candidate-v1.patch', 'build-candidate.py', 'candidate-build.json',
         'prepare-idle-fixture.py', 'idle-compile.json', 'idle-candidate',
         'idle-candidate.d', 'source-proof.md', 'freeze-code-review.py',
         'verify-code-review.py', 'fixtures/t-jit-idle-reclaim-entry.c',
         'fixtures/lib/lua_fixture_helpers.h', 'candidate/src/lj_tab.c',
         'candidate/src/lj_tab.h']
artifacts = {name: sha(p/name) for name in names}
out = p/'code-review-manifest.json'
assert not out.exists()
out.write_text(json.dumps({'count': len(artifacts), 'artifacts': artifacts,
                          'stage': 'frozen source/build review; no runtime tests yet'}, indent=2)+'\n')
print('manifest', len(artifacts), sha(out))
print('source proof', sha(p/'source-proof.md'))
