from pathlib import Path
import hashlib,json,stat
r=Path(__file__).resolve().parent
files={p for p in r.iterdir() if p.is_file() and p.name!='artifact-manifest.json'}
for d in r.iterdir():
 if d.is_dir() and d.name not in ['baseline','baseline-asan']:
  files.update(p for p in d.rglob('*') if p.is_file())
for v in ['baseline','baseline-asan']:
 for p in (r/v/'src').rglob('*'):
  if p.is_file() and (p.suffix in {'.c','.h','.S','.dasc','.lua','.def','.inc'} or p.name in {'libluajit.a','libluajit.so','luajit','buildvm','minilua'}):files.add(p)
for rel in ['tests/t-gc-root-pending.c','tests/t-gc-root-pending-race.c','notes/gc-root-pending-batch.md','notes/gc-root-pending-empty-flush-fastpath-2026-07-04.md','notes/gc-root-pending-transition-hint-2026-07-04.md']:
 files.add(r/'baseline'/rel)
mp=r/'artifact-manifest.json'
manifest={str(p.relative_to(r)):{'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'bytes':p.stat().st_size} for p in sorted(files)}
mp.write_text(json.dumps(manifest,indent=2)+'\n')
for p in r.rglob('*'):
 if p.is_file():p.chmod(stat.S_IMODE(p.stat().st_mode)&~0o222)
for p in [r/'HANDOFF.md',r/'PROPOSAL.md',r/'t-pending-root-eof.c',r/'source-verification.json',mp]:print(p.name,hashlib.sha256(p.read_bytes()).hexdigest(),p.stat().st_size,flush=True)
print('manifest_entries',len(manifest),flush=True)
