from pathlib import Path
import hashlib,json,shutil
r=Path(__file__).resolve().parent;e=r/'evidence';e.mkdir(exist_ok=True);manifest={'package':str(r),'copied':{},'hash_only':{}}
def digest(p):return dict(sha256=hashlib.sha256(p.read_bytes()).hexdigest(),bytes=p.stat().st_size)
for p in sorted(r.iterdir()):
 if not p.is_file():continue
 if p.suffix in ['.c','.h','.lua','.py','.patch','.json','.stdout','.stderr','.md']:
  d=e/'package'/p.name;d.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,d);manifest['copied'][str(d.relative_to(e))]=dict(source=str(p),**digest(p))
 elif p.read_bytes()[:4]==b'\x7fELF':manifest['hash_only'][str(p)]=digest(p)
for n in ['lj_obj.h','lj_gc.h','lj_gc.c','lj_gc2.c','lj_api.c','vm_x64.dasc']:
 p=r/'veto/src'/n;d=e/'modified-source/src'/n;d.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,d);manifest['copied'][str(d.relative_to(e))]=dict(source=str(p),**digest(p))
for p in sorted((r/'independent-review').rglob('*')):
 if p.is_file():
  d=e/'independent-review'/p.relative_to(r/'independent-review');d.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,d);manifest['copied'][str(d.relative_to(e))]=dict(source=str(p),**digest(p))
for v in ['veto','strict','asan','extrahelpers','controlextrahelpers']:
 for n in ['luajit','libluajit.a','host/minilua','host/buildvm']:
  p=r/v/'src'/n
  if p.exists():manifest['hash_only'][str(p)]=digest(p)
for q in ['/tmp/lj-gc-auto-admission-20260905-h7ntx71p/runtime-source.tar','/tmp/lj-gc-auto-admission-20260905-h7ntx71p/candidate.patch','/tmp/lj-gc-auto-admission-20260905-h7ntx71p/control/src/libluajit.a','/tmp/lj-gc-auto-admission-20260905-h7ntx71p/candidate/src/libluajit.a','/tmp/lj-gc-auto-admission-20260905-h7ntx71p/extrahelpers/src/libluajit.a']:
 p=Path(q);manifest['hash_only'][str(p)]=digest(p)
for rel,row in manifest['copied'].items():assert digest(e/rel)['sha256']==row['sha256']
p=e/'artifact-manifest.json';p.write_text(json.dumps(manifest,indent=2)+'\n');print(dict(path=str(p),sha256=digest(p)['sha256'],copied_files=len(manifest['copied']),copied_bytes=sum(x['bytes'] for x in manifest['copied'].values()),hash_only=len(manifest['hash_only'])))
