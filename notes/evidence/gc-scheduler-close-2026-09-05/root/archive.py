from pathlib import Path
import hashlib,json
p=Path(__file__).resolve().parent
o=Path('/tmp/lj-scheduler-terminal-close-20260905-nhl_nrgf')
r=Path('/workspaces/lj-lockless')
d=r/'notes/evidence/gc-scheduler-close-2026-09-05'
h=lambda b:hashlib.sha256(b).hexdigest()
rows=[]
seen=set()
def add(label,base,rel,expected=None,storage=None):
    key=str(Path(label)/rel)
    if key in seen:return
    assert not Path(rel).is_absolute() and '..' not in Path(rel).parts
    b=(base/rel).read_bytes();sha=h(b)
    if expected is not None:assert sha==expected,(base,rel)
    binary=b'\0' in b or b.startswith((b'\x7fELF',b'!<arch>\n'))
    if not binary:
        try:b.decode('utf8')
        except UnicodeDecodeError:binary=True
    if storage=='text':assert not binary,(base,rel)
    if storage=='hash-only':binary=True
    if not binary:
        f=d/key;f.parent.mkdir(parents=True,exist_ok=True);f.write_bytes(b);assert h(f.read_bytes())==sha
    rows.append(dict(source=str(base/rel),path=key,sha256=sha,bytes=len(b),storage='hash-only' if binary else 'text'))
    seen.add(key)
for name,sha in [('proposal-manifest.json','2204511b394ec389fccc7b8527c6cc7f24892b45ef7081a0c276fafcb32ce667'),('final-manifest.json','4fd711596b9c705bb0a4d890586268bc2ce3680ea4a0996358d7e0743de27f59')]:
    b=(o/name).read_bytes();assert h(b)==sha;m=json.loads(b)
    for row in m['artifacts']:
        assert (o/row['path']).stat().st_size==row['bytes']
        add('owner',o,row['path'],row['sha256'],row['storage'])
    add('owner',o,name,sha,'text')
can=json.loads((p/'canonical.json').read_text());assert can['exit']==0 and can['expected_runtime_processes']==3
assert h((r/'tests/t-gc2-worker-scheduler.c').read_bytes())==json.loads((p/'promotion.json').read_text())['after']
for f in sorted(p.rglob('*')):
    if f.is_file():add('root',p,str(f.relative_to(p)))
add('integrated',r,'tests/t-gc2-worker-scheduler.c')
totals=dict(text=sum(x['storage']=='text' for x in rows),hash_only=sum(x['storage']=='hash-only' for x in rows),text_bytes=sum(x['bytes'] for x in rows if x['storage']=='text'))
d.mkdir(parents=True,exist_ok=True)
(d/'manifest.json').write_text(json.dumps(dict(runtime_commit=json.loads((p/'promotion.json').read_text())['head'],totals=totals,entries=rows),indent=2)+'\n')
print(json.dumps(totals))
