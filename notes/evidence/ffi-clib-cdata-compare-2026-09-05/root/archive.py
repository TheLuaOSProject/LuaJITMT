from pathlib import Path
import hashlib, json

p = Path(__file__).resolve().parent
repo = Path('/workspaces/lj-lockless')
dest = repo/'notes/evidence/ffi-clib-cdata-compare-2026-09-05'
entries = []

def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def add(path, relative, expected=None, storage=None):
    digest = sha(path)
    if expected is not None:
        assert digest == expected, path
    data = path.read_bytes()
    if storage is None:
        if data.startswith((b'\x7fELF', b'!<arch>\n')) or b'\0' in data:
            storage = 'hash-only'
        else:
            try:
                data.decode('utf-8')
                storage = 'text'
            except UnicodeDecodeError:
                storage = 'hash-only'
    assert storage in ['text', 'hash-only'], (path, storage)
    if storage == 'text':
        assert not data.startswith((b'\x7fELF', b'!<arch>\n'))
        assert b'\0' not in data
        data.decode('utf-8')
        target = dest/relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        assert sha(target) == digest
    entries.append({'source': str(path), 'relative_path': str(relative),
                    'sha256': digest, 'bytes': len(data), 'storage': storage})

for directory, label, expected in [
    ('lj-clib-hit-cost-review-20260905-cmxsefzb', 'source-cost',
     '38c9329080373a95dbb843ef1baa16398c372a7c36dcefe7b10373dbf97e85e5'),
    ('lj-clib-cdata-compare-proof-20260905-ei_30vvn', 'code-ir-review',
     '19e60c095f33bcd56b0472a2f24c0b3272797836030c41b80a4f1dd29ca86ee8'),
    ('lj-clib-cdata-compare-20260905-mt2wayvj', 'isolated',
     'ca58863f5cda7f40ecc28b45a5b87b9f1ae1dc64185dd4eb3f8a21fc8501c01d')
]:
    owner = Path('/tmp')/directory
    manifest = owner/'artifact-manifest.json'
    assert sha(manifest) == expected
    data = json.loads(manifest.read_text())
    for entry in data['artifacts']:
        path = owner/entry['relative_path']
        assert path.stat().st_size == entry['bytes']
        add(path, Path(label)/entry['relative_path'], entry['sha256'],
            entry['storage'])
    add(manifest, Path(label)/'owner-artifact-manifest.json', expected, 'text')

identity = json.loads((p/'source-identity.json').read_text())
for kind in ['candidate', 'strict', 'asan']:
    for name, digest in identity[kind]['inputs'].items():
        assert sha(p/kind/name) == digest
        assert sha(repo/name) == digest
    for name in ['luajit', 'libluajit.a']:
        add(p/kind/'src'/name, Path('root/binary-identities')/kind/name,
            storage='hash-only')
validation = json.loads((p/'final-validation.json').read_text())
assert validation['combined_functional_total'] == 1174
assert validation['both_generations_functional_total'] == 1768
assert all(row['exit'] == 0 for row in validation['canonical'])
paths = [path for path in p.iterdir() if path.is_file()]
for folder in ['validation', 'canonical-tmp', 'perf']:
    paths.extend(path for path in (p/folder).rglob('*') if path.is_file())
for path in sorted(paths):
    add(path, Path('root')/path.relative_to(p))
for name in ['tests/suites/m7_ffi.lua',
             'tests/t-ffi-clib-cache-authority.lua',
             'tests/t-ffi-clib-cache-supplement.lua',
             'tests/t-ffi-clib-cache-between-close.c',
             'tests/t-ffi-clib-cdata-compare.c',
             'tests/t-ffi-clib-cdata-probe.lua',
             'tests/t-ffi-clib-cdata-retention.lua']:
    add(repo/name, Path('root/permanent-fixtures')/name, storage='text')
for name in ['luajit', 'libluajit.a', 'libluajit.so']:
    digest = validation['canonical'][-1]['default_binaries'][name]
    add(repo/'src'/name, Path('root/shared-binary-identities')/name,
        digest, 'hash-only')
manifest = {'artifacts': entries,
            'text_count': sum(e['storage'] == 'text' for e in entries),
            'hash_only_count': sum(e['storage'] == 'hash-only' for e in entries)}
(dest/'artifact-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')

bench = repo/'notes/bench/ffi-clib-cdata-compare-2026-09-05'
bench_entries = []
for owner, label in [(Path('/tmp/lj-clib-cdata-compare-20260905-mt2wayvj/perf'),
                       'isolated-aee'), (p/'perf', 'combined-843')]:
    for path in sorted(owner.iterdir()):
        if not path.is_file():
            continue
        data = path.read_bytes()
        data.decode('utf-8')
        assert b'\0' not in data
        target = bench/label/path.name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        bench_entries.append({'file': str(target.relative_to(bench)),
                              'sha256': sha(path), 'bytes': len(data),
                              'storage': 'text'})
(bench/'artifact-manifest.json').write_text(
    json.dumps({'artifacts': bench_entries}, indent=2)+'\n')
print(json.dumps({k: v for k, v in manifest.items() if k != 'artifacts'}))
