import hashlib, json, pathlib, shutil, subprocess

root = pathlib.Path('/workspaces/lj-lockless')
pkg = pathlib.Path(__file__).parent
owner = pathlib.Path('/tmp/lj-special-udata-independent-20260905-75jums60')
dest = root / 'notes/evidence/ffi-clib-receiver-2026-09-05'
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
entries = []

def add(p, rel, expected=None, binary=False):
    h = sha(p)
    if expected is not None:
        assert h == expected, (p, h, expected)
    e = {'source': str(p), 'relative_path': str(rel), 'sha256': h,
         'bytes': p.stat().st_size, 'storage': 'hash_only' if binary else 'text'}
    if not binary:
        target = dest / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(p, target)
        assert sha(target) == h
    entries.append(e)

manifest = json.loads((owner / 'artifact-manifest.json').read_text())
for e in manifest['artifacts']:
    add(pathlib.Path(e['source']), pathlib.Path('isolated') / e['relative_path'],
        e['sha256'], e['storage'] != 'text')
add(owner / 'artifact-manifest.json', pathlib.Path('isolated/artifact-manifest.json'))

source_manifest = json.loads((pkg / 'runtime-input-identity.json').read_text())
for p, h in source_manifest['exact_candidate_strict_asan'].items():
    assert sha(root / p) == h, p

validation = {'base': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
              'variants': {}, 'runtime_input_count': source_manifest['count']}
for variant in ('candidate', 'strict', 'asan'):
    rows = json.loads((pkg / (variant + '-results.json')).read_text())
    assert all(r['exit'] == 0 for r in rows), variant
    validation['variants'][variant] = {'commands': len(rows),
        'runtime_processes': sum(bool(r.get('test')) for r in rows), 'all_exit_zero': True}
canonical = json.loads((pkg / 'canonical.json').read_text())
assert canonical['exit'] == 0
for p, h in canonical['sources'].items():
    assert sha(root / p) == h, p
for p, h in canonical['default_binaries'].items():
    assert sha(root / 'src' / p) == h, p
validation['canonical'] = canonical
validation['owner_final_receiver_positives'] = 60
validation['owner_final_baseline_and_method_only_controls'] = {'success': 20, 'expected_native_failures': 20}
validation['shared_canonical_runtime_processes'] = 20
(pkg / 'final-validation.json').write_text(json.dumps(validation, indent=2) + '\n')
for p in sorted(pkg.iterdir()):
    if p.is_file():
        add(p, pathlib.Path('root') / p.name, binary=p.read_bytes()[:4] == b'\x7fELF')

out = {'base': validation['base'], 'artifacts': entries,
       'text_count': sum(e['storage'] == 'text' for e in entries),
       'hash_only_count': sum(e['storage'] != 'text' for e in entries)}
(dest / 'artifact-manifest.json').write_text(json.dumps(out, indent=2) + '\n')
print(json.dumps({k:v for k,v in out.items() if k != 'artifacts'}))
