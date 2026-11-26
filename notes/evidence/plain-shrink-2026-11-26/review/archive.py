from pathlib import Path
import hashlib,json,shutil,subprocess
p=Path(__file__).resolve().parent
root=Path('/workspaces/lj-lockless')
out=root/'notes/evidence/plain-shrink-2026-11-26'
out.mkdir(parents=True,exist_ok=False)
(out/'.gitattributes').write_text('# Preserve raw capture bytes, including CSV CRLF and diff context.\n* -whitespace\n')
manifest=[]
text_suffixes={'.py','.json','.jsonl','.txt','.c','.h','.lua','.csv','.log','.stdout','.stderr','.patch','.diff','.d','.sha256'}
def add(source,dest,copy=True):
 source=Path(source)
 if not source.is_file():return
 row=dict(source=str(source),path=str(dest),bytes=source.stat().st_size,sha256=hashlib.sha256(source.read_bytes()).hexdigest(),copied=copy)
 if copy:
  target=out/dest;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
 manifest.append(row)
patch=p/'source.patch'
paths=['src/lj_arena.c','src/lj_arena.h','tests/suites/m2_arena.lua','tests/t-arena-hugetab.c','tests/t-arena-realloc.c','tests/t-arena-allocf.c','tests/t-arena-plain-progress.c']
patch.write_bytes(subprocess.check_output(['git','diff','--',*paths],cwd=root))
for f in sorted(p.iterdir()):
 if f.is_file():add(f,Path('review')/f.name,f.suffix in text_suffixes)
add(root/'tests/t-arena-plain-shrink.c','review/t-arena-plain-shrink.c')
for label,folder in [('validation',p),('initial-validation',Path('/tmp/lj-plain-shrink-20261126-7izllbqs'))]:
 for tree in sorted(folder.iterdir()):
  if not tree.is_dir() or not (tree/'build.json').exists():continue
  for name in ['build.json','build.log']:add(tree/name,Path(label)/tree.name/name)
  for check in sorted(tree.glob('check-*')):
   for f in sorted(check.iterdir()):add(f,Path(label)/tree.name/check.name/f.name,f.suffix in {'.json','.stdout','.stderr','.d'})
  for name in ['luajit','libluajit.a']:add(tree/'src'/name,Path(label)/tree.name/name,False)
for f in sorted((p/'canonical').iterdir()):add(f,Path('canonical')/f.name)
add(root/'src/.lj-test-build-signature','canonical/default-build-signature.txt')
for label,folder in [('review/runtime',Path('/tmp/lj-small-plain-shrink-review.TI6NCx')),('performance',Path('/tmp/lj-plain-shrink-perf-20261126-s5650_mv'))]:
 for f in sorted(folder.rglob('*')):
  if not f.is_file():continue
  rel=f.relative_to(folder)
  copy=(f.suffix in text_suffixes or f.name in {'stdout','stderr','SHA256SUMS'}) and 'src' not in rel.parts
  add(f,Path(label)/rel,copy)
add('/tmp/lj-plain-shrink-perf-20261126-s5650_mv.tar.gz','performance/archive.tar.gz',False)
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
summary=dict(copied=sum(r['copied'] for r in manifest),hash_only=sum(not r['copied'] for r in manifest),copied_bytes=sum(r['bytes'] for r in manifest if r['copied']),manifest_sha256=hashlib.sha256((out/'manifest.json').read_bytes()).hexdigest())
(p/'archive-result.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
