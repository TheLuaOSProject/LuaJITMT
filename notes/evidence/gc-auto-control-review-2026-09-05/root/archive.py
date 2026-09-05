import hashlib, json, pathlib, shutil, subprocess

r = pathlib.Path('/workspaces/lj-lockless')
p = pathlib.Path(__file__).parent
initial = pathlib.Path('/tmp/lj-gc-auto-admission-20260905-h7ntx71p')
veto = pathlib.Path('/tmp/lj-gc-auto-stop-overlap-20260905-y4h4cc8a')
dest = r/'notes/evidence/gc-auto-control-review-2026-09-05'
entries = []

def sha(q):
    with q.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def add(q, rel, expected=None, text=True, source=None):
    h = sha(q)
    if expected is not None:
        assert h == expected, (q, h, expected)
    if text:
        data = q.read_bytes()
        assert not data.startswith((b'\x7fELF', b'!<arch>\n')), q
        assert b'\0' not in data, q
        data.decode('utf-8')
        target = dest/rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(q, target)
        assert sha(target) == h
    entries.append({'source': str(source or q), 'relative_path': str(rel),
                    'sha256': h, 'bytes': q.stat().st_size,
                    'storage': 'text' if text else 'hash_only'})

manifest_file = veto/'evidence/artifact-manifest.json'
assert sha(manifest_file) == 'ff02877adb7dafe854fbc8173e71ae3a3b0b2a1d6d773226bf182826454c90d8'
m = json.loads(manifest_file.read_text())
assert len(m['copied']) == 530 and len(m['hash_only']) == 76
for name, e in m['copied'].items():
    add(veto/'evidence'/name, pathlib.Path('stop-overlap')/name,
        e['sha256'], source=e['source'])
for i, (name, e) in enumerate(m['hash_only'].items()):
    q = pathlib.Path(name)
    add(q, pathlib.Path('stop-overlap/binary-identities')/str(i)/q.name,
        e['sha256'], text=False)
add(manifest_file, pathlib.Path('stop-overlap/owner-artifact-manifest.json'))

# The initial owner did not publish a classified artifact manifest. Preserve
# every top-level output; ELF/archives remain hash-only. Reject any other
# non-text input rather than treating an unknown storage label as text.
for q in sorted(initial.iterdir()):
    if not q.is_file():
        continue
    with q.open('rb') as f:
        head = f.read(8)
    binary = head.startswith((b'\x7fELF', b'!<arch>\n', b'\x1f\x8b')) or q.suffix == '.tar'
    add(q, pathlib.Path('initial-admission')/q.name, text=not binary)

for directory, label, expected in [
    ('/tmp/lj-gc-auto-admission-proof-20260905-mrosx7pd', 'initial-review',
     'f51f88bad0eab080d74b82137437aeb160ae863f698c3906f00feb6cf6d1b94d'),
    ('/tmp/lj-gc-auto-stop-veto-proof-20260905-gqkrx2u7', 'veto-review',
     '09e728914a79566861a6ff4c2cbb0975538aebbdb5c85ed89a3fd09d35ab9007'),
]:
    owner = pathlib.Path(directory)
    manifest = owner/'artifact-manifest.json'
    assert sha(manifest) == expected
    for e in json.loads(manifest.read_text())['files']:
        add(owner/e['path'], pathlib.Path(label)/e['path'], e['sha256'])
    add(manifest, pathlib.Path(label)/'artifact-manifest.json')

base = '597b8705208957ade8465416da30976ab9b52195'
names = ['src/lj_api.c', 'src/lj_gc.c', 'src/lj_gc.h', 'src/lj_gc2.c',
         'src/lj_obj.h', 'src/vm_x64.dasc', 'src/lib_threading.c', 'src/lj_str.c']
for name in names:
    old = subprocess.check_output(['git', 'show', base+':'+name], cwd=r)
    assert hashlib.sha256(old).hexdigest() == sha(r/name), name
verification = {
    'shared_head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=r, text=True).strip(),
    'frozen_gc_base': base,
    'unchanged_shared_gc_threading_string_inputs': {name: sha(r/name) for name in names},
    'production_change_integrated': False,
    'original_three_file_admission_candidate': 'Unsafe: reproduced completed-before-call STOP violation.',
    'six_file_stop_veto': 'Source-reviewed bounded STOP repair; RESTART/attachment loss remains '
                         'reproduced and scheduler failure remains unresolved. No integration approval.',
    'subsequent_work': 'A separate isolated authoritative-control repair and scheduler diagnosis '
                       'are in progress; their future results do not replace these frozen observations.',
}
(p/'verification.json').write_text(json.dumps(verification, indent=2)+'\n')
for q in sorted(p.iterdir()):
    if q.is_file():
        add(q, pathlib.Path('root')/q.name)
dest.mkdir(parents=True, exist_ok=True)
manifest = {'artifacts': entries, 'text_count': sum(e['storage']=='text' for e in entries),
            'hash_only_count': sum(e['storage']=='hash_only' for e in entries)}
(dest/'artifact-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
print(json.dumps({k:v for k,v in manifest.items() if k != 'artifacts'}))
