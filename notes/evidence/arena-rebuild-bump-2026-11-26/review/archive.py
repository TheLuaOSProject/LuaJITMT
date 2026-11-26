from pathlib import Path
import hashlib, json, shutil, subprocess

p = Path(__file__).resolve().parent
root = Path('/workspaces/lj-lockless')
out = root/'notes/evidence/arena-rebuild-bump-2026-11-26'
out.mkdir(parents=True, exist_ok=False)
(out/'.gitattributes').write_text('# Preserve raw capture bytes, including CSV CRLF and diff context.\n* -whitespace\n')
manifest = []
destinations = set()
text_suffixes = {'.py', '.json', '.jsonl', '.txt', '.md', '.c', '.h', '.lua',
                 '.csv', '.log', '.stdout', '.stderr', '.patch', '.diff', '.d', '.sha256'}
def add(source, dest, copy=True):
    source = Path(source)
    if not source.is_file(): return
    dest = str(dest)
    assert dest not in destinations, dest
    destinations.add(dest)
    row = dict(source=str(source), path=dest, bytes=source.stat().st_size,
               sha256=hashlib.sha256(source.read_bytes()).hexdigest(), copied=copy)
    if copy:
        target = out/dest
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    manifest.append(row)

paths = ['src/lj_arena.c', 'tests/suites/m2_arena.lua']
(p/'source.patch').write_bytes(subprocess.check_output(['git', 'diff', '--', *paths], cwd=root))
for f in sorted(p.iterdir()):
    if f.is_file(): add(f, Path('review')/f.name, f.suffix in text_suffixes)
add(root/'tests/t-arena-rebuild-bump.c', 'review/t-arena-rebuild-bump.c')
for mode in ['gcc', 'assert', 'asan', 'tsan', 'noffi', 'nojit', 'nobuf']:
    tree = p/mode
    for name in ['build.json', 'build.log']:
        add(tree/name, Path('validation')/mode/name)
    for check in sorted(tree.glob('check-*')):
        for f in sorted(check.iterdir()):
            add(f, Path('validation')/mode/check.name/f.name,
                f.suffix in {'.json', '.stdout', '.stderr', '.d'})
    for name in ['luajit', 'libluajit.a']:
        add(tree/'src'/name, Path('validation')/mode/name, False)
for label in ['canonical']:
    for f in sorted((p/label).iterdir()): add(f, Path(label)/f.name)
add(root/'src/.lj-test-build-signature', 'canonical/default-build-signature.txt')
for label, folder in [
    ('rebuild-fixture', p/'rebuild-fixture'),
    ('review/runtime', Path('/tmp/lj-arena-rebuild-bump-review.yycd0k1l')),
    ('review/ownership', Path('/tmp/lj-arena-rebuild-bump-audit-20261126-mf173ld2'))]:
    for f in sorted(folder.rglob('*')):
        if f.is_file():
            copy = f.suffix in text_suffixes or f.name in {'stdout', 'stderr', 'SHA256SUMS'}
            add(f, Path(label)/f.relative_to(folder), copy)
(out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
summary = dict(copied=sum(r['copied'] for r in manifest),
    hash_only=sum(not r['copied'] for r in manifest),
    copied_bytes=sum(r['bytes'] for r in manifest if r['copied']),
    manifest_sha256=hashlib.sha256((out/'manifest.json').read_bytes()).hexdigest())
(p/'archive-result.json').write_text(json.dumps(summary, indent=2)+'\n')
print(json.dumps(summary, indent=2))
