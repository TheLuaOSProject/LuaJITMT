from pathlib import Path
import hashlib, json

repo = Path('/workspaces/lj-lockless')
p = Path(__file__).resolve().parent
dest = repo/'notes/evidence/gc-construction-defer-review-2026-09-05'
entries = {}

def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def add(path, relative, digest, size, storage):
    assert sha(path) == digest and path.stat().st_size == size, path
    assert storage in ['text', 'hash-only'], (path, storage)
    entry = {'source': str(path), 'relative_path': str(relative),
             'sha256': digest, 'bytes': size, 'storage': storage}
    if str(relative) in entries:
        assert entries[str(relative)] == entry
        return
    if storage == 'text':
        data = path.read_bytes()
        data.decode('utf-8')
        assert b'\0' not in data
        assert not data.startswith((b'\x7fELF', b'!<arch>\n'))
        target = dest/relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        assert sha(target) == digest
    entries[str(relative)] = entry

for directory, name, label, digest in [
    ('lj-func-construction-timeout-20260905-8htcwnmd', 'manifest.json', 'diagnosis',
     '520715455c2b82e9ad800621bf03be1a1300957ccf5b521d9a2419f94895532e'),
    ('lj-reclaim-owner-defer-20260905-gwiiudxk', 'focused-manifest.json', 'rejected',
     '1c95ae02cd52c065634b7fc6a68c34ed8544c2106a5c34d7e7215ab4b4fde1ee'),
    ('lj-reclaim-owner-defer-20260905-gwiiudxk', 'acceptance-manifest.json', 'rejected',
     '30aed87c54ccf6274ecda0b5948363c5c83ae59991a59db9e5924f1965ec53d7')
]:
    owner = Path('/tmp')/directory
    manifest = owner/name
    assert sha(manifest) == digest
    data = json.loads(manifest.read_text())
    for entry in data['artifacts']:
        add(owner/entry['path'], Path(label)/entry['path'],
            entry['sha256'], entry['bytes'], entry['storage'])
    add(manifest, Path(label)/('owner-'+name), digest,
        manifest.stat().st_size, 'text')

for path, relative in [
    (p/'archive.py', Path('root/archive.py')),
    (repo/'notes/gc-construction-defer-review-2026-09-05.md', Path('root/review.md'))
]:
    add(path, relative, sha(path), path.stat().st_size, 'text')
manifest = {'status': 'diagnosis and rejected candidate; no runtime integration',
            'artifacts': list(entries.values()),
            'text_count': sum(e['storage'] == 'text' for e in entries.values()),
            'hash_only_count': sum(e['storage'] == 'hash-only' for e in entries.values())}
(dest/'artifact-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
print(json.dumps({k: v for k, v in manifest.items() if k != 'artifacts'}))
