#!/usr/bin/env python3
"""Archive only verified text; identify executable artifacts by hash."""
import hashlib
import json
import pathlib
import subprocess

root = pathlib.Path('/tmp/lj-helper-fixtures-root-20260905-wa27ui28')
owner = pathlib.Path('/tmp/lj-gc-helper-assertions-20260905-z9obuha4')
idle = pathlib.Path('/tmp/lj-idle-entry-timeout-20260905-1tin1mhx')
repo = pathlib.Path('/workspaces/lj-lockless')
dest = repo / 'notes/evidence/gc-helper-fixtures-2026-09-05'

def sha(data):
    return hashlib.sha256(data).hexdigest()

verification = subprocess.run(['python3', str(owner / 'verify-final.py')],
                              cwd=repo, capture_output=True, text=True)
(root / 'owner-verification.stdout').write_text(verification.stdout)
(root / 'owner-verification.stderr').write_text(verification.stderr)
assert verification.returncode == 0, verification.stderr
expected = {
    'artifact-manifest.json': '5aad0e6bde498e1f93c9591c14ed7e765c7394df2d6dbc1095396a84308a8921',
    'final-review.md': '948107a22abcef747fc6e145bade231eadf8763919af303d7d56f14707618d62',
    'final-verification.json': 'bea84bea2d30c581ccf730a6600762458b57892576956bd9c7d2135860ae0691',
}
for name, digest in expected.items():
    assert sha((owner / name).read_bytes()) == digest, name
manifest = json.loads((owner / 'artifact-manifest.json').read_text())
assert len(manifest['artifacts']) == manifest['count'] == 141
entries = []
seen = set()

def add(label, base, rel, expected_sha=None):
    key = str(pathlib.PurePosixPath(label) / rel)
    if key in seen:
        return
    assert '..' not in pathlib.PurePosixPath(rel).parts
    source = base / rel
    data = source.read_bytes()
    digest = sha(data)
    if expected_sha is not None:
        assert digest == expected_sha, source
    entry = dict(source=str(source), path=key, sha256=digest, bytes=len(data))
    binary = (b'\0' in data or data.startswith(b'\x7fELF') or
              data.startswith(b'!<arch>\n'))
    if not binary:
        try:
            data.decode('utf-8')
        except UnicodeDecodeError:
            binary = True
    if binary:
        entry['storage'] = 'hash-only'
    else:
        target = dest / key
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        assert sha(target.read_bytes()) == digest
        entry['storage'] = 'text'
    entries.append(entry)
    seen.add(key)

for name, digest in manifest['artifacts'].items():
    assert isinstance(digest, str) and len(digest) == 64
    add('owner', owner, name, digest)
for name, digest in expected.items():
    add('owner', owner, name, digest)
for base, label in [(root, 'root'), (idle, 'idle-diagnostic')]:
    for source in sorted(base.rglob('*')):
        if source.is_file():
            add(label, base, source.relative_to(base).as_posix())
for item in json.loads((root / 'promotion.json').read_text()):
    assert sha((repo / 'tests' / item['name']).read_bytes()) == item['after']
totals = {
    'text': sum(x['storage'] == 'text' for x in entries),
    'hash_only': sum(x['storage'] == 'hash-only' for x in entries),
    'text_bytes': sum(x['bytes'] for x in entries if x['storage'] == 'text'),
}
out = dict(runtime_commit='79345529bd932e68f8159ec17224467a10cad09b',
           canonical_head='e3b2ec6afc4f6819fad7fad84dc179c250196155',
           owner_manifest=expected['artifact-manifest.json'], totals=totals,
           entries=entries)
dest.mkdir(parents=True, exist_ok=True)
(dest / 'manifest.json').write_text(json.dumps(out, indent=2) + '\n')
print(json.dumps(totals))
