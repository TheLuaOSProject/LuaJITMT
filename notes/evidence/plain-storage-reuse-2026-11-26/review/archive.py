from pathlib import Path
import hashlib,json,shutil,subprocess
p=Path(__file__).resolve().parent;root=Path('/workspaces/lj-lockless')
out=root/'notes/evidence/plain-storage-reuse-2026-11-26';out.mkdir(parents=True,exist_ok=False)
(out/'.gitattributes').write_text('# Preserve raw capture bytes, including CSV CRLF and diff context.\n* -whitespace\n')
manifest=[]
def sha(f):return hashlib.sha256(f.read_bytes()).hexdigest()
def add(src,rel,copy=True):
 src=Path(src)
 if not src.is_file():return
 row={'source':str(src),'path':str(rel),'bytes':src.stat().st_size,'sha256':sha(src),'copied':copy}
 if copy:
  dest=out/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dest)
 manifest.append(row)
for f in sorted(p.iterdir()):
 if not f.is_file():continue
 copy=f.suffix in ['.py','.json','.c','.stdout','.stderr']
 if copy or f.name.startswith(('plain-late-','live-growth-')):add(f,Path('review')/f.name,copy)
patch=p/'source.patch';patch.write_bytes(subprocess.check_output(['git','diff','--','src/lj_arena.c','src/lj_arena.h','src/lj_safepoint.c','tests/suites/m2_arena.lua'],cwd=root))
add(patch,'review/source.patch');add(root/'tests/t-arena-plain-late.c','review/t-arena-plain-late.c')
oldtest=p/'candidate-gcc/tests/t-arena-plain-late.c'
assert sha(oldtest)==json.loads((p/'baseline.json').read_text())['test_sha256']
add(oldtest,'review/baseline-test.c')
for tree in sorted(p.iterdir()):
 if not tree.is_dir() or not (tree/'build.json').exists():continue
 for name in ['build.json','build.log']:add(tree/name,Path('validation')/tree.name/name)
 for folder in sorted(tree.glob('check-*')):
  for f in sorted(folder.iterdir()):
   add(f,Path('validation')/tree.name/folder.name/f.name,copy=f.suffix in ['.json','.stdout','.stderr','.d'])
 for name in ['luajit','libluajit.a']:add(tree/'src'/name,Path('validation')/tree.name/name,copy=False)
for f in sorted((p/'canonical').iterdir()):add(f,Path('canonical')/f.name)
add(root/'src/.lj-test-build-signature','canonical/default-build-signature.txt')
for label,path in [('diagnosis','/tmp/lj-buffer-churn-growth-20261126-fu6iac_1'),('forced-scan','/tmp/lj-buffer-churn-fix-20261126-sr3y4cv_'),('final','/tmp/lj-buffer-churn-final-20261126-rmdi4q4b')]:
 folder=Path(path)
 for f in sorted(folder.rglob('*')):
  if not f.is_file():continue
  rel=f.relative_to(folder)
  copy=f.suffix in ['.json','.jsonl','.txt','.csv','.lua','.py','.c','.h','.log','.stdout','.stderr','.sha256','.patch'] or f.name=='SHA256SUMS'
  add(f,Path('performance')/label/rel,copy)
 add(Path(path+'.tar.gz'),Path('performance')/(label+'.tar.gz'),copy=False)
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
report={'copied':sum(r['copied'] for r in manifest),'hash_only':sum(not r['copied'] for r in manifest),'text_bytes':sum(r['bytes'] for r in manifest if r['copied']),'manifest_sha256':sha(out/'manifest.json')}
(p/'archive-result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
