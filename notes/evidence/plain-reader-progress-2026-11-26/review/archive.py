from pathlib import Path
import hashlib, json, shutil, subprocess
p = Path(__file__).resolve().parent
root = Path('/workspaces/lj-lockless')
out = root/'notes/evidence/plain-reader-progress-2026-11-26'
out.mkdir(parents=True, exist_ok=False)
(out/'.gitattributes').write_text('# Preserve raw capture bytes, including CSV CRLF and diff context.\n* -whitespace\n')
manifest = []
def sha(f): return hashlib.sha256(f.read_bytes()).hexdigest()
def add(source, dest, copy=True):
    source = Path(source)
    if not source.is_file(): return
    row = dict(source=str(source), path=str(dest), bytes=source.stat().st_size,
               sha256=sha(source), copied=copy)
    if copy:
        target = out/dest
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    manifest.append(row)
text_suffixes = {'.py','.json','.jsonl','.txt','.c','.h','.lua','.csv','.log',
                 '.stdout','.stderr','.patch','.diff','.d','.sha256'}
for f in sorted(p.iterdir()):
    if f.is_file(): add(f, Path('review')/f.name, f.suffix in text_suffixes)
baseline = json.loads((p/'baseline.json').read_text())
assert sha(p/'baseline-test.c') == baseline['source_sha256']
patch = p/'source.patch'
patch.write_bytes(subprocess.check_output(['git','diff','--','src/lj_arena.c',
    'src/lj_arena.h','tests/suites/m2_arena.lua','tests/t-arena-hugetab.c'], cwd=root))
add(patch, 'review/source.patch')
for name in ['t-arena-plain-progress.c','t-arena-plain-bin-progress.c']:
    add(root/'tests'/name, Path('review')/name)
for tree in sorted(p.iterdir()):
    if not tree.is_dir() or not (tree/'build.json').exists(): continue
    for name in ['build.json','build.log']:
        add(tree/name, Path('validation')/tree.name/name)
    for folder in sorted(tree.glob('check-*')):
        for f in sorted(folder.iterdir()):
            add(f, Path('validation')/tree.name/folder.name/f.name,
                f.suffix in {'.json','.stdout','.stderr','.d'})
    for name in ['luajit','libluajit.a']:
        add(tree/'src'/name, Path('validation')/tree.name/name, False)
for f in sorted((p/'canonical').iterdir()): add(f, Path('canonical')/f.name)
add(root/'src/.lj-test-build-signature', 'canonical/default-build-signature.txt')
for label, folder in [
    ('review/readers', Path('/tmp/lj-plain-reader-audit-20261126-j10ngz7d')),
    ('review/final', Path('/tmp/lj-plain-reader-final-review-20261126-vwojwnwh')),
    ('review/bin-baseline', Path('/tmp/lj-plain-bin-progress-20261126-h5z9l2rs')),
    ('performance', Path('/tmp/lj-plain-progress-perf-20261126-sucpc_rf'))]:
    for f in sorted(folder.rglob('*')):
        if not f.is_file(): continue
        rel = f.relative_to(folder)
        copy = (f.suffix in text_suffixes or f.name == 'SHA256SUMS') and 'src' not in rel.parts
        add(f, Path(label)/rel, copy)
add('/tmp/lj-plain-progress-perf-20261126-sucpc_rf.tar.gz', 'performance/archive.tar.gz', False)
(out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
summary = dict(copied=sum(r['copied'] for r in manifest),
               hash_only=sum(not r['copied'] for r in manifest),
               copied_bytes=sum(r['bytes'] for r in manifest if r['copied']),
               manifest_sha256=sha(out/'manifest.json'))
(p/'archive-result.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
