from pathlib import Path
import hashlib,json

p=Path(__file__).resolve().parent
r=Path('/workspaces/lj-lockless')
d=r/'notes/evidence/tab-scalar-next-2026-09-05'
d.mkdir(exist_ok=False)
h=lambda b:hashlib.sha256(b).hexdigest()
rows=[]
seen=set()
def add(label,base,rel,expected=None):
    assert not Path(rel).is_absolute() and '..' not in Path(rel).parts
    key=str(Path(label)/rel)
    assert key not in seen,key
    b=(base/rel).read_bytes();digest=h(b)
    if expected is not None:assert digest==expected,(base,rel)
    binary=b'\0' in b or b.startswith((b'\x7fELF',b'!<arch>\n'))
    if not binary:
        try:b.decode('utf8')
        except UnicodeDecodeError:binary=True
    if not binary:
        f=d/key;f.parent.mkdir(parents=True,exist_ok=True);f.write_bytes(b)
    rows.append(dict(source=str(base/rel),path=key,sha256=digest,bytes=len(b),
        storage='hash-only' if binary else 'text'))
    seen.add(key)

packages=[
 ('owner',Path('/tmp/lj-idle-scalar-next-20260905-zsvtsqsn'),
  '2f526f18874696539c19965fe8db862dee4afbbce42a956afe08d894c216cd85','files'),
 ('proof',Path('/tmp/lj-idle-scalar-next-proof-20260905-c53dodz1'),
  '446902a5f6082cf4b6804a204716989e56aabb258b61e29666d440a18071a847','artifacts')]
verified={}
for label,base,digest,key in packages:
    b=(base/'artifact-manifest.json').read_bytes();assert h(b)==digest
    entries=json.loads(b)[key]
    for rel,expected in entries.items():add(label,base,rel,expected)
    add(label,base,'artifact-manifest.json',digest)
    verified[label]=dict(manifest_sha256=digest,verified_artifacts=len(entries))
for f in sorted(p.iterdir()):
    if f.is_file():add('root',p,f.name)
for sub in ['fixtures','results-candidate','results-optimized','results-strict',
            'results-asan','results-baseline','canonical-tmp','perf']:
    for f in sorted((p/sub).rglob('*')):
        if f.is_file():add('root',p,str(f.relative_to(p)))
for rel in ['src/lj_tab.c','src/lj_tab.h','tests/suites/m5_tables.lua',
            'tests/suites/m5_aggregate.lua','tests/t-tab-scalar-next-progress.c',
            'tests/t-tab-scalar-next-authority.c','tests/t-tab-scalar-next-stack-retry.c',
            'tests/t-tab-scalar-next-lifetime.c','tests/t-jit-idle-reclaim-entry.c']:
    add('integrated',r,rel)
stop=Path('/tmp/lj-api-capture-validation-stop-20260905-gvwlmj0n')
for rel in ['README.md','status.json']:add('related-capi-stop',stop,rel)
totals=dict(text=sum(x['storage']=='text' for x in rows),
    hash_only=sum(x['storage']=='hash-only' for x in rows),
    text_bytes=sum(x['bytes'] for x in rows if x['storage']=='text'))
manifest=dict(base_head=json.loads((p/'setup.json').read_text())['head'],
    status='scalar-array next accepted on worker+fair source; public C capture excluded',
    package_verification=verified,totals=totals,entries=rows)
(d/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
for e in rows:
    if e['storage']=='text':
        b=(d/e['path']).read_bytes();assert h(b)==e['sha256'] and len(b)==e['bytes']
        assert b'\0' not in b and not b.startswith((b'\x7fELF',b'!<arch>\n'));b.decode('utf8')
print(json.dumps(dict(totals=totals,verified=verified,manifest_sha256=h((d/'manifest.json').read_bytes()))))
