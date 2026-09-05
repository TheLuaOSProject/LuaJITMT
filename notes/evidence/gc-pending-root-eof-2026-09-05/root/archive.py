from pathlib import Path
import hashlib,json
p=Path(__file__).resolve().parent
o=Path('/tmp/lj-gc-pending-root-design-20260905-blju2qsh')
r=Path('/workspaces/lj-lockless')
d=r/'notes/evidence/gc-pending-root-eof-2026-09-05'
sha=lambda b:hashlib.sha256(b).hexdigest()
rows=[]
def add(label,base,rel,expected=None,size=None):
    assert not Path(rel).is_absolute() and '..' not in Path(rel).parts
    b=(base/rel).read_bytes();digest=sha(b)
    if expected is not None:assert digest==expected,(base,rel)
    if size is not None:assert len(b)==size,(base,rel)
    binary=b'\0' in b or b.startswith((b'\x7fELF',b'!<arch>\n'))
    if not binary:
        try:b.decode('utf8')
        except UnicodeDecodeError:binary=True
    key=str(Path(label)/rel)
    if not binary:
        dest=d/key;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(b)
        assert sha(dest.read_bytes())==digest
    rows.append(dict(source=str(base/rel),path=key,sha256=digest,bytes=len(b),storage='hash-only' if binary else 'text'))
b=(o/'artifact-manifest.json').read_bytes()
assert sha(b)=='a585b1be68eb073ab8da7eb3d921c6eed3910084d131825d9233c1ef97e28213'
for rel,e in json.loads(b).items():add('owner',o,rel,e['sha256'],e['bytes'])
add('owner',o,'artifact-manifest.json',sha(b))
for f in sorted(p.iterdir()):
    if f.is_file():add('root',p,f.name)
totals=dict(text=sum(x['storage']=='text' for x in rows),hash_only=sum(x['storage']=='hash-only' for x in rows),text_bytes=sum(x['bytes'] for x in rows if x['storage']=='text'))
(d/'manifest.json').write_text(json.dumps(dict(runtime_base='eb8a5b2f9ce2fd6128f4dbeef25b03896b81cfcd',status='diagnosis and source-only proposal; no implementation',totals=totals,entries=rows),indent=2)+'\n')
print(json.dumps(totals))
