import hashlib, json, pathlib, shutil

r = pathlib.Path('/workspaces/lj-lockless')
p = pathlib.Path(__file__).parent
v = p/'v2'
dest = r/'notes/evidence/ffi-clib-cache-authority-2026-09-05'
bench = r/'bench/ffi-clib-cache-authority-2026-09-05'
sha = lambda q: hashlib.sha256(q.read_bytes()).hexdigest()
entries = []

def is_text(q):
    data = q.read_bytes()
    assert not data.startswith((b'\x7fELF', b'!<arch>\n')), q
    assert b'\0' not in data, q
    data.decode('utf-8')

def add(q, rel, expected=None, storage='text'):
    h = sha(q)
    if expected is not None:
        assert h == expected, (q, h, expected)
    if storage == 'text':
        is_text(q)
        target = dest/rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(q, target)
        assert sha(target) == h
    else:
        assert storage == 'hash_only', storage
    entries.append({'source': str(q), 'relative_path': str(rel), 'sha256': h,
                    'bytes': q.stat().st_size, 'storage': storage})

owners = [
    ('/tmp/lj-clib-cache-regressions-20260905-741ke1nb', 'isolated',
     '880d0c16fd8cee50456eed2490970f4f3ba546014283d7da4be5d0b0cac038e2'),
    ('/tmp/lj-clib-cache-close-proof-20260905-o4j8qsz5', 'design-review', None),
    ('/tmp/lj-clib-hit-try-proof-20260905-gl2f_616', 'source-review',
     '8cd12b1712bcbd57a993b33e539c429d3fd230f4831358558887d4e2dbe9e46e'),
]
for dirname, label, expected_manifest in owners:
    owner = pathlib.Path(dirname)
    manifest_file = owner/'artifact-manifest.json'
    if expected_manifest:
        assert sha(manifest_file) == expected_manifest
    manifest = json.loads(manifest_file.read_text())
    for e in manifest['artifacts']:
        name = e.get('relative_path', e.get('path'))
        q = pathlib.Path(e.get('source', owner/name))
        storage = e['storage']
        # This older review's nine explicitly listed source/document entries
        # use "content". Normalize only that known schema after checking bytes.
        if label == 'source-review' and storage == 'content':
            assert q.suffix in ['.c', '.h', '.patch', '.md', '.json'], q
            is_text(q)
            storage = 'text'
        assert storage in ['text', 'hash-only', 'hash_only', 'sha256-only'], e
        add(q, pathlib.Path(label)/name, e['sha256'],
            'text' if storage == 'text' else 'hash_only')
    add(manifest_file, pathlib.Path(label)/'artifact-manifest.json')

identity = json.loads((v/'source-identity.json').read_text())
assert identity['count'] == len(identity['inputs']) == 224
for name, h in identity['inputs'].items():
    assert sha(r/name) == h, name
    for variant in ['candidate', 'strict', 'asan']:
        assert sha(v/variant/name) == h, (variant, name)

validation = {}
for variant in ['candidate', 'strict', 'asan']:
    build = json.loads((v/(variant+'-build.json')).read_text())
    broad = json.loads((v/(variant+'-results.json')).read_text())
    roots = json.loads((v/(variant+'-recorder-roots-results.json')).read_text())
    assert build['exit'] == 0
    assert all(row['exit'] == 0 for row in broad+roots)
    assert sum(bool(row.get('test')) for row in broad) == 98
    assert sum(bool(row.get('test')) for row in roots) == 5
    validation[variant] = {'build_exit': 0, 'broad_commands': len(broad),
                           'broad_runtime_processes': 98, 'recorder_commands': len(roots),
                           'recorder_runtime_processes': 5}
canonical = json.loads((p/'canonical.json').read_text())
assert canonical['exit'] == 0 and canonical['expected_runtime_processes'] == 153
for name, h in canonical['sources'].items():
    assert sha(r/name) == h, name
for name, h in canonical['default_binaries'].items():
    assert sha(r/'src'/name) == h, name
isolated = json.loads((pathlib.Path(owners[0][0])/'handoff-summary.json').read_text())
assert isolated['combined_qualified_runtime_passes'] == 444
cost = json.loads((v/'perf/cost-results.json').read_text())
shapes = json.loads((v/'native-shapes.json').read_text())
assert len(cost) == 70 and all(row['exit'] == 0 for row in cost)
assert len(shapes) == 2 and all(row['exit'] == 0 for row in shapes)
final = {
    'integration': json.loads((p/'integration.json').read_text()),
    'all_224_runtime_inputs_match_shared_and_three_variants': True,
    'root_validation': validation, 'root_runtime_passes': 309,
    'independent_runtime_passes': 444, 'canonical_runtime_passes': 153,
    'qualified_functional_runtime_passes': 906,
    'separate_native_shape_probes': 2, 'separate_cost_processes': 70,
    'exact_baseline_controls': isolated['exact_5c455f20_controls'],
    'canonical': canonical,
    'limits': [
        'The extra attachment-during-recording fixture was blocked by automatic '
        'safety review citing possible cybersecurity risk and was not performed. '
        'The source protocol review and earlier combined first-activation tests '
        'remain separately recorded; this is not a claim of that new schedule.',
        'Public full-GC lifetime checks do not prove collector overlap inside the helper.',
        'The generated helper attempts bounded admission but publication can retry CAS '
        'or grow GC queues. No allocation-free, wait-free or fixed-cost claim.',
        'A raced pre-MT recorder attempt may use the generic waiting sampler before '
        'the new post-check rejects it; that helper cannot be installed here.',
        'General shared-MT metamethod recording and full stock performance parity remain open.',
        'Linux x64 validation only; no release readiness claim.',
    ],
}
(p/'final-validation.json').write_text(json.dumps(final, indent=2)+'\n')

for q in sorted(p.iterdir()):
    if q.is_file():
        add(q, pathlib.Path('root')/q.name)
for q in sorted(v.iterdir()):
    if q.is_file():
        storage = 'text' if q.suffix in ['.json', '.py', '.c', '.stdout', '.stderr'] else 'hash_only'
        add(q, pathlib.Path('root/v2')/q.name, storage=storage)
for variant in ['candidate', 'strict', 'asan']:
    tree = v/variant
    for name in ['luajit', 'libluajit.a', 'libluajit.so', 'host/minilua', 'host/buildvm',
                 'lj_crecord.o', 'lj_ir.o']:
        q = tree/'src'/name
        if q.is_file():
            add(q, pathlib.Path('root/v2')/variant/'src'/name, storage='hash_only')
    add(tree/'src/jit/vmdef.lua', pathlib.Path('root/v2')/variant/'src/jit/vmdef.lua')
for name in ['src/lj_crecord.c', 'src/lj_ircall.h', 'src/lj_opt_mem.c']:
    add(v/'candidate'/name, pathlib.Path('root/v2/source')/name)
    add(p/'candidate'/name, pathlib.Path('root/v1/source')/name)
for name in ['luajit', 'libluajit.a', 'libluajit.so']:
    q = p/'candidate/src'/name
    if q.is_file():
        add(q, pathlib.Path('root/v1/src')/name, storage='hash_only')
for q in sorted((p/'canonical-tmp').rglob('*')):
    if q.is_file():
        add(q, pathlib.Path('root')/q.relative_to(p), storage='hash_only')
add(pathlib.Path('/tmp/lj-special-udata-independent-20260905-75jums60/clib-cache-lifecycle.lua'),
    pathlib.Path('root/initial-clib-cache-lifecycle.lua'))
for name, h in canonical['default_binaries'].items():
    add(r/'src'/name, pathlib.Path('root/default-binaries')/name, h, 'hash_only')

manifest = {'artifacts': entries, 'text_count': sum(e['storage']=='text' for e in entries),
            'hash_only_count': sum(e['storage']=='hash_only' for e in entries)}
dest.mkdir(parents=True, exist_ok=True)
(dest/'artifact-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
bench.mkdir(parents=True, exist_ok=True)
bench_entries = []
for q in sorted((v/'perf').iterdir()):
    if not q.is_file():
        continue
    is_text(q)
    shutil.copyfile(q, bench/q.name)
    bench_entries.append({'source': str(q), 'relative_path': q.name, 'sha256': sha(q),
                          'bytes': q.stat().st_size, 'storage': 'text'})
(bench/'artifact-manifest.json').write_text(json.dumps({'artifacts': bench_entries}, indent=2)+'\n')
print(json.dumps({'text_count': manifest['text_count'],
                  'hash_only_count': manifest['hash_only_count'],
                  'bench_text_count': len(bench_entries),
                  'functional_passes': final['qualified_functional_runtime_passes']}))
