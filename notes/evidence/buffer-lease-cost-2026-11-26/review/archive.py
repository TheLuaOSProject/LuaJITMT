from pathlib import Path
import hashlib,json,shutil,subprocess,sys
p=Path(__file__).resolve().parent;root=Path('/workspaces/lj-lockless')
out=root/'notes/evidence/buffer-lease-cost-2026-11-26'
out.mkdir(parents=True,exist_ok=False)
manifest=[]
def sha(f):return hashlib.sha256(f.read_bytes()).hexdigest()
def add(src,rel,copy=True):
 src=Path(src)
 if not src.is_file():return
 row={'source':str(src),'path':str(rel),'bytes':src.stat().st_size,'sha256':sha(src),'copied':copy}
 if copy:
  dest=out/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dest)
 manifest.append(row)
for f in sorted(p.glob('*.py')):add(f,Path('review')/f.name)
for name in ['setup.json','final-verification.json']:
 add(p/name,Path('review')/name)
owned=['src/lj_buf.c','src/lj_buf.h','src/lj_gc2.c','tests/t-buffer-progress.c','tests/suites/m3_gc.lua']
patch=p/'source.patch'
patch.write_bytes(subprocess.check_output(['git','diff','--',*owned],cwd=root))
add(patch,'review/source.patch')
add(root/'tests/t-gc2-small-lease-span.c','review/t-gc2-small-lease-span.c')
for mode in ['capacity-v1','combined-v1','assert-v1','asan-v1','tsan-v1','noffi-v1','nojit-v1','nobuf-v1']:
 tree=p/mode
 for name in ['build.json','build.log']:add(tree/name,Path('validation')/mode/name)
 for folder in sorted(tree.glob('check-*')):
  for f in sorted(folder.iterdir()):
   add(f,Path('validation')/mode/folder.name/f.name,copy=f.suffix in ['.json','.stdout','.stderr','.d'])
 for name in ['luajit','libluajit.a']:add(tree/'src'/name,Path('validation')/mode/name,copy=False)
for f in sorted((p/'canonical').iterdir()):add(f,Path('canonical')/f.name)
for label,folder in [('capacity','/tmp/lj-buffer-perf-capacity-20261126-x8cbe3v3'),('combined','/tmp/lj-buffer-perf-combined-20261126-payfa8fl')]+[(Path(x).name,x) for x in sys.argv[1:]]:
 folder=Path(folder)
 for f in sorted(folder.rglob('*')):
  if not f.is_file():continue
  # Runtime source snapshots and executables stay hash-only. Drivers, raw rows,
  # reports and profiles rendered to text remain reviewable in Git.
  rel=f.relative_to(folder)
  if any(part in ['baseline','candidate','stock','src','dynasm','doc','etc'] for part in rel.parts):continue
  copy=(f.suffix in ['.json','.txt','.csv','.lua','.py','.c','.h','.log','.stdout','.stderr','.sha256','.patch','.asm','.dis','.dump'] or f.name in ['stdout','stderr'])
  add(f,Path('performance')/label/rel,copy)
 add(Path(str(folder)+'.tar.gz'),Path('performance')/(label+'.tar.gz'),copy=False)
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps({'copied':sum(r['copied'] for r in manifest),'hash_only':sum(not r['copied'] for r in manifest),'text_bytes':sum(r['bytes'] for r in manifest if r['copied']),'manifest_sha256':sha(out/'manifest.json')},indent=2))
