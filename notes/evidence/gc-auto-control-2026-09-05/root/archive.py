from pathlib import Path
import hashlib, json, shutil, subprocess

p = Path(__file__).resolve().parent
repo = Path('/workspaces/lj-lockless')
dest = repo/'notes/evidence/gc-auto-control-2026-09-05'
entries = []

def sha(q):
    with q.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def add(q, rel, expected=None, text=True, source=None):
    h = sha(q)
    if expected is not None:
        assert h == expected, (q, h, expected)
    if text:
        data = q.read_bytes()
        assert not data.startswith((b'\x7fELF', b'!<arch>\n')), q
        assert b'\0' not in data, q
        data.decode('utf-8')
        target = dest/rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        assert sha(target) == h
    entries.append({'source': str(source or q), 'relative_path': str(rel),
                    'sha256': h, 'bytes': q.stat().st_size,
                    'storage': 'text' if text else 'hash_only'})

for directory, label, expected, counts in [
    ('/tmp/lj-gc-auto-control-20260905-qs673ryl', 'candidate2',
     '5bdc2b9431e12a3d769d579b2ab09da3e68074b72258650b9c1fe81274351300', (780, 85)),
    ('/tmp/lj-gc-auto-query-20260905-k7a7wved', 'candidate3',
     'd57a61bcc2bd862ab0de5d41b89738840a89b65b5c264e7c6d0ca30a8503514f', (489, 44))
]:
    owner = Path(directory)
    manifest = owner/'evidence/artifact-manifest.json'
    assert sha(manifest) == expected
    data = json.loads(manifest.read_text())
    assert (len(data['copied']), len(data['hash_only'])) == counts
    for name, e in data['copied'].items():
        add(owner/'evidence'/name, Path(label)/name, e['sha256'], source=e['source'])
    for i, (name, e) in enumerate(data['hash_only'].items()):
        q = Path(name)
        add(q, Path(label)/'binary-identities'/str(i)/q.name,
            e['sha256'], text=False)
    add(manifest, Path(label)/'owner-artifact-manifest.json')

source = json.loads((p/'source-identity.json').read_text())
assert all(sha(repo/n) == h for n,h in source['sources'].items())
broad = {}
for kind in ['candidate', 'strict', 'asan']:
    rows = json.loads((p/(kind+'-results.json')).read_text())
    runtimes = [x for x in rows if x['test']]
    assert len(runtimes) == 98 and all(x['exit'] == 0 for x in rows)
    broad[kind] = {'runtime_processes': len(runtimes), 'failed_commands': 0}
canonical = json.loads((p/'canonical.json').read_text())
assert len(canonical) == 2 and all(x['exit'] == 0 for x in canonical)
validation = {'shared_base': source['head'], 'runtime_inputs_verified': len(source['sources']),
              'broad': broad, 'canonical': [{k:x[k] for k in
              ['name','exit','seconds','expected_runtime_processes','default_binaries']}
              for x in canonical], 'current_candidate_owner_runtimes': 151,
              'current_candidate_owner_passes': 124,
              'current_candidate_owner_worker_two_incompletions': 27,
              'total_current_positive_functional_runtimes': 608,
              'benchmark_processes': 70, 'calibration_processes': 6,
              'known_open': ['synchronous GC driver and phase ownership',
                            'worker-two SWEEP completion', 'concurrent string reclamation',
                            'earlier helper assertions and constructor/IDLE waits']}
(p/'final-validation.json').write_text(json.dumps(validation, indent=2)+'\n')

# ROOT trees contain generated objects and are represented by source/binary
# identities. Copy only top-level evidence and explicit fixture/tmp artifacts.
paths = [q for q in p.iterdir() if q.is_file()]
for folder in ['fixtures', 'canonical-tmp']:
    paths.extend(q for q in (p/folder).rglob('*') if q.is_file())
for q in sorted(paths):
    with q.open('rb') as stream:
        prefix = stream.read(8)
    binary = prefix.startswith((b'\x7fELF', b'!<arch>\n')) or q.suffix == '.tar'
    add(q, Path('root')/q.relative_to(p), text=not binary)
manifest = {'artifacts': entries,
            'text_count': sum(e['storage'] == 'text' for e in entries),
            'hash_only_count': sum(e['storage'] == 'hash_only' for e in entries)}
(dest/'artifact-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')

bench = repo/'notes/bench/gc-auto-control-2026-09-05'
bench.mkdir(parents=True, exist_ok=True)
bench_entries = []
for name in ['allocation-cost.lua', 'benchmark.py', 'cost-pilot.json',
             'cost-results.json', 'cost-summary.json']:
    q = p/name
    data = q.read_bytes()
    data.decode('utf-8')
    assert b'\0' not in data
    (bench/name).write_bytes(data)
    bench_entries.append({'file': name, 'sha256': sha(q), 'bytes': len(data), 'storage': 'text'})
(bench/'artifact-manifest.json').write_text(json.dumps({'artifacts': bench_entries}, indent=2)+'\n')
print(json.dumps({k:v for k,v in manifest.items() if k != 'artifacts'}))
