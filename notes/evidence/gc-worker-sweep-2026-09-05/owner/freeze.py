from pathlib import Path
import hashlib,json,stat
r=Path(__file__).resolve().parent
variants=['baseline','baseline-helpers','baseline-asan-helpers','candidate','candidate2','candidate2-assert','candidate2-asan','candidate2-helpers','candidate2-asan-helpers']
files={p for p in r.iterdir() if p.is_file() and p.name!='artifact-manifest.json'}
for d in r.iterdir():
 if d.is_dir() and d.name not in variants:
  files.update(p for p in d.rglob('*') if p.is_file())
base=json.loads((r/'base-identity.json').read_text())
for rel in base['files']:
 if rel.startswith(('src/','dynasm/','tests/')):
  p=r/'baseline'/rel
  if p.is_file():files.add(p)
for v in variants:
 d=r/v/'src'
 for p in d.rglob('*'):
  if p.is_file() and (p.suffix in {'.c','.h','.S','.dasc','.lua','.def','.inc'} or p.name in {'libluajit.a','libluajit.so','luajit','buildvm','minilua'}):files.add(p)
manifest={str(p.relative_to(r)):{'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'bytes':p.stat().st_size} for p in sorted(files)}
mp=r/'artifact-manifest.json';mp.write_text(json.dumps(manifest,indent=2)+'\n')
# Freeze all evidence, including source trees; any later experiment must use a new package.
for p in r.rglob('*'):
 if p.is_file():p.chmod(stat.S_IMODE(p.stat().st_mode)&~0o222)
mp.chmod(0o444)
for p in [r/'HANDOFF.md',r/'SOURCE-PROOF.md',r/'REPRODUCTION.md',r/'source-verification.json',mp]:
 print(p.name,hashlib.sha256(p.read_bytes()).hexdigest(),p.stat().st_size,flush=True)
print('manifest_entries',len(manifest),flush=True)
