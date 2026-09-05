from pathlib import Path
import hashlib, json
p=Path(__file__).resolve().parent
o=Path('/tmp/lj-gc-worker-bridge-20260905-bheds8jz')
prior=Path('/tmp/lj-gc-worker-sweep-20260905-2xv7dsqc')
r=Path('/workspaces/lj-lockless')
d=r/'notes/evidence/gc-worker-sweep-2026-09-05'
h=lambda b:hashlib.sha256(b).hexdigest()
rows=[]
seen=set()
def add(label,base,rel,expected=None,size=None,storage=None):
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
    if storage=='text':assert not binary,(base,rel)
    if storage=='hash-only':binary=True
    if not binary:
        f=d/key;f.parent.mkdir(parents=True,exist_ok=True)
        if not f.exists():f.write_bytes(b)
        assert h(f.read_bytes())==sha,(base,rel)
    rows.append(dict(source=str(base/rel),path=key,sha256=sha,bytes=len(b),storage='hash-only' if binary else 'text'))
    seen.add(key)
name='artifact-manifest.json'
b=(o/name).read_bytes();assert h(b)=='824ecb98680341f5c925fa07cf963622dcfd09cba3fe29c50c6c931808aa1faa'
for rel,e in json.loads(b).items():add('owner',o,rel,e['sha256'],e['bytes'])
add('owner',o,name,h(b),storage='text')
b=(prior/'evidence'/name).read_bytes();assert h(b)=='81cd21fd4cc4ba112c9d4eb86755b19598a69d3020c3ee561c08085386112aa2'
m=json.loads(b)
for rel,e in m['text_artifacts'].items():add('diagnosis',prior/'evidence',rel,e['sha256'],e['bytes'],'text')
for rel,e in m['binary_identities'].items():add('diagnosis',prior,rel,e['sha256'],e['bytes'],'hash-only')
add('diagnosis',prior/'evidence',name,h(b),storage='text')
for f in sorted(p.iterdir()):
    if f.is_file():add('root',p,f.name)
for sub in ['fixtures','results-candidate','results-strict','results-asan','perf','canonical-tmp','scheduler-failure']:
    if (p/sub).exists():
        for f in sorted((p/sub).rglob('*')):
            if f.is_file():add('root',p,str(f.relative_to(p)))
for rel in ['src/lj_gc2.c','tests/suites/m3_gc.lua',
            'tests/t-worker-bridge-stop.c','tests/t-worker-bridge-detach.c',
            'tests/t-worker-bridge-consumed.c','tests/t-worker-bridge-quiet.c',
            'tests/t-string-retention.c','tests/peer-control.lua',
            'tests/t-safepoint-local-native-duplicate.c']:
    add('integrated',r,rel)
totals=dict(text=sum(x['storage']=='text' for x in rows),hash_only=sum(x['storage']=='hash-only' for x in rows),text_bytes=sum(x['bytes'] for x in rows if x['storage']=='text'))
(d/'manifest.json').write_text(json.dumps(dict(base_head=json.loads((p/'setup.json').read_text())['head'],
    status='integration under review; canonical scheduler failure preserved',totals=totals,entries=rows),indent=2)+'\n')
print(json.dumps(totals))
