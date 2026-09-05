from pathlib import Path
import hashlib, json, shutil

p = Path(__file__).resolve().parent
sha = lambda f: hashlib.sha256(Path(f).read_bytes()).hexdigest()
original = Path('/tmp/lj-helper-fixtures-root-20260905-wa27ui28')
for name in ['canonical.json', 'm6_jit_alloc_account.stdout', 'm6_jit_alloc_account.stderr']:
    dest = p/'original'/name
    assert not dest.exists()
    shutil.copy2(original/name, dest)
manifest = p/'artifact-manifest.json'
assert not manifest.exists()
artifacts = {str(f.relative_to(p)): sha(f) for f in sorted(p.rglob('*'))
             if f.is_file() and f.name != 'final-verification.json'}
manifest.write_text(json.dumps({'count': len(artifacts), 'artifacts': artifacts,
                               'stage': 'source proof and proposal only; no candidate implementation'}, indent=2)+'\n')
print('manifest', len(artifacts), sha(manifest))
print('source-proposal.md', sha(p/'source-proposal.md'))
print('handoff.md', sha(p/'handoff.md'))
