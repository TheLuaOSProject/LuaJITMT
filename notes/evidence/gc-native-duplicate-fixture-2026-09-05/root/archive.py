from pathlib import Path
import hashlib, json
p=Path(__file__).resolve().parent
o=Path('/tmp/lj-gc-worker-bridge-20260905-bheds8jz')
r=Path('/workspaces/lj-lockless')
d=r/'notes/evidence/gc-native-duplicate-fixture-2026-09-05'
h=lambda b:hashlib.sha256(b).hexdigest()
rows=[]
seen=set()
def add(label,base,rel,expected=None,size=None):
    key=str(Path(label)/rel)
    if key in seen:return
    assert not Path(rel).is_absolute() and '..' not in Path(rel).parts
    b=(base/rel).read_bytes();sha=h(b)
    if expected is not None:assert sha==expected,(base,rel)
    if size is not None:assert len(b)==size,(base,rel)
    binary=b'\0' in b or b.startswith((b'\x7fELF',b'!<arch>\n'))
    if not binary:
        try:b.decode('utf8')
        except UnicodeDecodeError:binary=True
    if not binary:
        f=d/key;f.parent.mkdir(parents=True,exist_ok=True);f.write_bytes(b);assert h(f.read_bytes())==sha
    rows.append(dict(source=str(base/rel),path=key,sha256=sha,bytes=len(b),storage='hash-only' if binary else 'text'))
    seen.add(key)
for name,sha in [('duplicate-v2-manifest.json','06986f8f72a7b1bc4a5ee293d275ab48067028e118b62d82bfc5d7bba8cefc5a'),
                 ('duplicate-diagnostic-manifest.json','a58670f755158a9bfdca910bf5fb1db10e81557278a68e1bafaec44637c4315b')]:
    b=(o/name).read_bytes();assert h(b)==sha
    for rel,e in json.loads(b).items():add('owner',o,rel,e['sha256'],e['bytes'])
    add('owner',o,name,sha)
owner_manifest=json.loads((o/'artifact-manifest.json').read_text())
for rel in ['candidate2-helpers-existing-t-safepoint-local-native-duplicate-0.stderr',
            'candidate2-helpers-existing-t-safepoint-local-native-duplicate-0.stdout',
            'candidate2-full.patch','source-v2-manifest.json','SOURCE-PROOF.md','source-verification.json']:
    e=owner_manifest[rel];add('owner',o,rel,e['sha256'],e['bytes'])
for f in sorted(p.rglob('*')):
    if f.is_file():add('root',p,str(f.relative_to(p)))
add('integrated',r,'tests/t-safepoint-local-native-duplicate.c',
    'f8f0ce140091d4105419177d5c3bb9299858debc46d6f628d8aeceea944cdc4a')
assert json.loads((p/'canonical.json').read_text())['exit']==0
for kind in ['strict','asan']:assert json.loads((p/kind/'summary.json').read_text())['runtime_pass']==2
totals=dict(text=sum(x['storage']=='text' for x in rows),hash_only=sum(x['storage']=='hash-only' for x in rows),text_bytes=sum(x['bytes'] for x in rows if x['storage']=='text'))
(d/'manifest.json').write_text(json.dumps(dict(runtime_commit='eb8a5b2f',totals=totals,entries=rows),indent=2)+'\n')
print(json.dumps(totals))
