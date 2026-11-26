from pathlib import Path
import hashlib,json,shutil
root=Path('/workspaces/lj-lockless')
p=Path(__file__).resolve().parent
out=root/'notes/evidence/worker-retention-2026-11-26'
out.mkdir(parents=True,exist_ok=False)
(out/'.gitattributes').write_text('# Preserve raw capture bytes, including CSV CRLF.\n* -whitespace\n')
manifest=[]
text_suffixes={'.py','.json','.jsonl','.txt','.c','.h','.lua','.csv','.log','.stdout','.stderr','.patch','.diff','.d','.sha256'}
def add(source,target,copy):
 source=Path(source)
 if not source.is_file(): return
 row=dict(source=str(source),path=str(target),bytes=source.stat().st_size,sha256=hashlib.sha256(source.read_bytes()).hexdigest(),copied=copy)
 if copy:
  dest=out/target;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,dest)
 manifest.append(row)
for label,folder in [('census',p),('public',Path('/tmp/lj-multiworker-buffer-20261126-o6j07pmh')),('ownership',Path('/tmp/lj-plain-worker-ownership-20261126-fg7_swl4'))]:
 for f in sorted(folder.rglob('*')):
  if not f.is_file():continue
  rel=f.relative_to(folder)
  copy=(f.suffix in text_suffixes or f.name in {'stdout','stderr','SHA256SUMS'}) and 'src' not in rel.parts and 'tests' not in rel.parts
  add(f,Path(label)/rel,copy)
add('/tmp/lj-multiworker-buffer-20261126-o6j07pmh.tar.gz',Path('public/archive.tar.gz'),False)
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
summary=dict(copied=sum(r['copied'] for r in manifest),hash_only=sum(not r['copied'] for r in manifest),copied_bytes=sum(r['bytes'] for r in manifest if r['copied']),manifest_sha256=hashlib.sha256((out/'manifest.json').read_bytes()).hexdigest())
(p/'archive-result.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
