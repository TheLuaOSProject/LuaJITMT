from pathlib import Path
import hashlib, json

p = Path(__file__).resolve().parent
r = Path('/workspaces/lj-lockless')
d = r/'notes/evidence/gc-worker-sweep-2026-09-05'
h = lambda b: hashlib.sha256(b).hexdigest()
initial_bytes = (d/'manifest.json').read_bytes()
initial = json.loads(initial_bytes)
assert initial['status'] == 'integration under review; canonical scheduler failure preserved'
assert initial['totals'] == dict(text=4191, hash_only=190, text_bytes=164903063)
rows = initial['entries'].copy()
seen = {x['path']:x for x in rows}
assert len(seen) == len(rows)
for e in rows:
    if e['storage'] == 'text':
        b = (d/e['path']).read_bytes()
        assert len(b) == e['bytes'] and h(b) == e['sha256'], e['path']

def add(label, base, rel, expected=None, size=None, storage=None):
    assert not Path(rel).is_absolute() and '..' not in Path(rel).parts
    key = str(Path(label)/rel)
    b = (base/rel).read_bytes()
    digest = h(b)
    if expected is not None:
        assert digest == expected, (base,rel)
    if size is not None:
        assert len(b) == size, (base,rel)
    binary = b'\0' in b or b.startswith((b'\x7fELF',b'!<arch>\n'))
    if not binary:
        try:
            b.decode('utf8')
        except UnicodeDecodeError:
            binary = True
    if storage == 'text':
        assert not binary, (base,rel)
    if storage == 'hash-only':
        binary = True
    row = dict(source=str(base/rel),path=key,sha256=digest,bytes=len(b),
               storage='hash-only' if binary else 'text')
    if key in seen:
        old = seen[key]
        assert all(old[k] == row[k] for k in ['path','sha256','bytes','storage']), key
        return
    if not binary:
        f = d/key
        f.parent.mkdir(parents=True,exist_ok=True)
        if not f.exists():
            f.write_bytes(b)
        assert h(f.read_bytes()) == digest, key
    rows.append(row)
    seen[key] = row

snapshot = d/'manifest-pre-integration.json'
assert not snapshot.exists()
snapshot.write_bytes(initial_bytes)
add('',d,snapshot.name,h(initial_bytes),len(initial_bytes),'text')
packages = [
    ('scheduler-diagnosis',Path('/tmp/lj-scheduler-worker-abort-20260905-wz2vi031'),
     'manifest.json','c761ae3765c9490ce432463a6974304329e3afc36c9b7dc52adfb3012b22b9de','artifacts'),
    ('scheduler-correction',Path('/tmp/lj-scheduler-ready-authority-20260905-ceov_nqr'),
     'manifest.json','550d64cc329f191f20c8854b59165694ea52c79c2a3e5eb7074dd45a9b2d0217','artifacts'),
    ('jit-matrix',Path('/tmp/lj-worker-jit-matrix-20260905-a6_4rjri'),
     'artifact-manifest.json','e64ca159e9742fd23f2622617336aab3fed2eb05348a8b5f34d8a8a745dfceb1','entries'),
    ('jit-diagnosis',Path('/tmp/lj-jit-sweep-diagnosis-20260905-jjdidw9u'),
     'artifact-manifest.json','407576f5b20f575a802918e517605a9f18522293fc9532d75ff170b0d54d416b','entries'),
]
verified = {}
for label, base, name, digest, field in packages:
    b = (base/name).read_bytes()
    assert h(b) == digest, (base,name)
    entries = json.loads(b)[field]
    if isinstance(entries,dict):
        for rel,e in entries.items():
            add(label,base,rel,e['sha256'],e['bytes'])
    else:
        for e in entries:
            add(label,base,e['path'],e['sha256'],e['bytes'],e.get('storage',e.get('mode')))
    add(label,base,name,digest,len(b),'text')
    verified[label] = dict(manifest_sha256=digest,verified_entries=len(entries))

for f in sorted(p.iterdir()):
    if f.is_file():
        add('root',p,f.name)
for f in sorted((p/'canonical-scheduler-v2').rglob('*')):
    if f.is_file():
        add('root',p,str(f.relative_to(p)))
for rel in ['tests/t-gc2-worker-scheduler.c','tests/t-gc-workers.lua']:
    add('integrated-final',r,rel)
for label,base in [('owner',Path('/tmp/lj-gc-worker-bridge-20260905-bheds8jz')),
                   ('diagnosis',Path('/tmp/lj-gc-worker-sweep-20260905-2xv7dsqc'))]:
    if f'{label}/HANDOFF.md' not in seen:
        add(label,base,'HANDOFF.md',storage='text')

totals = dict(text=sum(x['storage']=='text' for x in rows),
              hash_only=sum(x['storage']=='hash-only' for x in rows),
              text_bytes=sum(x['bytes'] for x in rows if x['storage']=='text'))
result = dict(base_head=initial['base_head'],
              status='worker+fair runtime and corrected scheduler accepted; baseline JIT assistance defect remains open',
              prior_manifest_sha256=h(initial_bytes),
              added_package_verification=verified,totals=totals,entries=rows)
(d/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')
for e in rows:
    if e['storage']=='text':
        b=(d/e['path']).read_bytes()
        assert b'\0' not in b and not b.startswith((b'\x7fELF',b'!<arch>\n'))
        b.decode('utf8')
        assert h(b)==e['sha256'] and len(b)==e['bytes'], e['path']
print(json.dumps(dict(totals=totals,added_packages=verified,manifest_sha256=h((d/'manifest.json').read_bytes()))))
