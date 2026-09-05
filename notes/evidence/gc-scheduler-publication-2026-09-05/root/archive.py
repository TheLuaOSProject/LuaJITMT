import hashlib, json, pathlib, shutil

r = pathlib.Path('/workspaces/lj-lockless')
p = pathlib.Path(__file__).parent
dest = r/'notes/evidence/gc-scheduler-publication-2026-09-05'
sha = lambda q: hashlib.sha256(q.read_bytes()).hexdigest()
entries = []

def add(q, rel, expected=None, storage='text'):
    h = sha(q)
    if expected is not None:
        assert h == expected, (q, h, expected)
    if storage == 'text':
        data = q.read_bytes()
        assert not data.startswith((b'\x7fELF', b'!<arch>\n')) and b'\0' not in data, q
        data.decode('utf-8')
        target = dest/rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(q, target)
        assert sha(target) == h
    else:
        assert storage == 'hash_only', storage
    entries.append({'source': str(q), 'relative_path': str(rel), 'sha256': h,
                    'bytes': q.stat().st_size, 'storage': storage})

for dirname, label, name, expected in [
    ('/tmp/lj-gc-scheduler-review-20260905-zuvqerrl', 'diagnosis', 'diagnosis-manifest.json',
     '1a6c0967081e3e7e1c64df329f42fb0971dd8a9631ba6ba379b7652471b07652'),
    ('/tmp/lj-gc-scheduler-publication-20260905-2zckpem_', 'correction', 'manifest.json',
     '96da0944ddc83bda91de3dd6cfce288036e232a474ca8496ba5603a3ecef456d'),
]:
    owner = pathlib.Path(dirname)
    mf = owner/name
    assert sha(mf) == expected
    for e in json.loads(mf.read_text())['artifacts']:
        assert e['storage'] in ['text', 'hash-only'], e
        q = pathlib.Path(e.get('original', owner/e['path']))
        add(q, pathlib.Path(label)/e['path'], e['sha256'],
            'text' if e['storage']=='text' else 'hash_only')
    add(mf, pathlib.Path(label)/name)

canonical = json.loads((p/'canonical.json').read_text())
assert canonical['exit'] == 0 and canonical['expected_runtime_processes'] == 3
for name, h in canonical['sources'].items():
    assert sha(r/name) == h, name
for name, h in canonical['default_binaries'].items():
    add(r/'src'/name, pathlib.Path('root/default-binaries')/name, h, 'hash_only')
for name, h in canonical['temporary_files'].items():
    add(p/name, pathlib.Path('root')/name, h, 'hash_only')
validation = {
    'runtime_source_changed': False,
    'fixture_sha256': sha(r/'tests/t-gc2-worker-scheduler.c'),
    'isolated_corrected_runtime_passes': 60,
    'intentional_publication_and_drain_negative_controls': 6,
    'diagnostic_runtime_processes': 120,
    'diagnostic_passes': 113, 'matched_original_assertion_failures': 7,
    'canonical_runtime_passes': 3, 'canonical': canonical,
    'scope': 'A fixture owner-publication precondition, not a worker-runtime repair. '
             'The original failure did not record internal state; newly diagnosed '
             'failures establish the same assertion on all three frozen variants.',
}
(p/'final-validation.json').write_text(json.dumps(validation, indent=2)+'\n')
for q in sorted(p.iterdir()):
    if q.is_file():
        add(q, pathlib.Path('root')/q.name)
dest.mkdir(parents=True, exist_ok=True)
manifest = {'artifacts': entries, 'text_count': sum(e['storage']=='text' for e in entries),
            'hash_only_count': sum(e['storage']=='hash_only' for e in entries)}
(dest/'artifact-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
print(json.dumps({k:v for k,v in manifest.items() if k!='artifacts'}))
