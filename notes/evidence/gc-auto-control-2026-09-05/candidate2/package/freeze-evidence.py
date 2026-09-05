from pathlib import Path
import hashlib, json, shutil
r=Path(__file__).resolve().parent
e=r/'evidence'
assert not e.exists(), 'Evidence is immutable; create a new package.'
e.mkdir()
manifest={'package':str(r),'copied':{},'hash_only':{}}
def digest(p):return dict(sha256=hashlib.sha256(p.read_bytes()).hexdigest(),bytes=p.stat().st_size)
def copy(p, rel):
 d=e/rel
 d.parent.mkdir(parents=True,exist_ok=True)
 shutil.copy2(p,d)
 manifest['copied'][str(rel)]=dict(source=str(p),**digest(p))
for p in sorted(r.iterdir()):
 if not p.is_file():continue
 if p.suffix in ['.c','.h','.lua','.py','.patch','.json','.stdout','.stderr','.md']:
  copy(p,Path('package')/p.name)
 elif p.read_bytes()[:4]==b'\x7fELF':manifest['hash_only'][str(p)]=digest(p)
for n in ['lj_obj.h','lj_gc.h','lj_gc.c','lj_gc2.c','lj_api.c','vm_x64.dasc']:
 copy(r/'candidate2/src'/n,Path('modified-source/src')/n)
for p in sorted((r/'independent-review').rglob('*')):
 if p.is_file():copy(p,Path('independent-review')/p.relative_to(r/'independent-review'))
for v in ['control','candidate','candidate2','strict','asan','helpers','controlhelpers']:
 for n in ['luajit','libluajit.a','host/minilua','host/buildvm']:
  p=r/v/'src'/n
  if p.exists():manifest['hash_only'][str(p)]=digest(p)
manifest['hash_only'][str(r/'runtime-source.tar')]=digest(r/'runtime-source.tar')
tests=set()
for p in r.glob('*results.json'):
 rows=json.loads(p.read_text())
 if not isinstance(rows,list):continue
 for row in rows:
  for arg in row.get('argv',[]):
   q=Path(arg)
   if q.is_file() and q.is_relative_to(r) and '/tests/' in str(q) and q.suffix in ['.c','.lua']:
    rel=str(q).split('/tests/',1)[1]
    tests.add(rel)
for rel in sorted(tests):copy(r/'candidate2/tests'/rel,Path('existing-tests')/rel)
for n in ['lib/test_sleep.h','lib/thread_harness.lua']:
 p=r/'candidate2/tests'/n
 if p.exists():copy(p,Path('existing-tests')/n)
for rel,row in manifest['copied'].items():assert digest(e/rel)['sha256']==row['sha256']
p=e/'artifact-manifest.json'
p.write_text(json.dumps(manifest,indent=2)+'\n')
print(dict(path=str(p),sha256=digest(p)['sha256'],copied_files=len(manifest['copied']),copied_bytes=sum(x['bytes'] for x in manifest['copied'].values()),hash_only=len(manifest['hash_only'])))
