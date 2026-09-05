from pathlib import Path
import hashlib,json
p=Path(__file__).resolve().parent
o=Path('/tmp/lj-jit-foreground-design-20260905-hwhdaa4a')
d=Path('/workspaces/lj-lockless/notes/evidence/gc-jit-foreground-design-2026-09-05')
d.mkdir(exist_ok=False)
h=lambda b:hashlib.sha256(b).hexdigest()
rows=[]
def add(label,base,rel,expected=None,size=None):
    assert not Path(rel).is_absolute() and '..' not in Path(rel).parts
    b=(base/rel).read_bytes();digest=h(b)
    if expected is not None:assert digest==expected,rel
    if size is not None:assert len(b)==size,rel
    binary=b'\0' in b or b.startswith((b'\x7fELF',b'!<arch>\n'))
    if not binary:
        try:b.decode('utf8')
        except UnicodeDecodeError:binary=True
    path=str(Path(label)/rel)
    if not binary:
        f=d/path;f.parent.mkdir(parents=True,exist_ok=True);f.write_bytes(b)
    rows.append(dict(source=str(base/rel),path=path,sha256=digest,bytes=len(b),storage='hash-only' if binary else 'text'))
b=(o/'artifact-manifest.json').read_bytes()
assert h(b)=='8d9bb4a2e6757d479253b58a8a2e879d96af95ccfc730d646730c7b5864b8d3e'
for rel,e in json.loads(b)['entries'].items():add('owner',o,rel,e['sha256'],e['bytes'])
add('owner',o,'artifact-manifest.json',h(b),len(b))
for f in sorted(p.iterdir()):
    if f.is_file():add('root',p,f.name)
totals=dict(text=sum(x['storage']=='text' for x in rows),hash_only=sum(x['storage']=='hash-only' for x in rows),text_bytes=sum(x['bytes'] for x in rows if x['storage']=='text'))
(d/'manifest.json').write_text(json.dumps(dict(status='source-only design; no implementation or runtime validation',totals=totals,entries=rows),indent=2)+'\n')
for e in rows:
    if e['storage']=='text':
        b=(d/e['path']).read_bytes();assert h(b)==e['sha256'] and len(b)==e['bytes'];assert b'\0' not in b;b.decode('utf8')
print(json.dumps(totals))
