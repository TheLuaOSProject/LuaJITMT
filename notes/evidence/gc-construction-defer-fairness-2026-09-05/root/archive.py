from pathlib import Path
import hashlib, json, subprocess

p = Path(__file__).resolve().parent
repo = Path('/workspaces/lj-lockless')
focused = Path('/tmp/lj-reclaim-fair-pass-20260905-kw8kfdam')
broad = Path('/tmp/lj-reclaim-fair-validation-20260905-c2edwuno')
proof = Path('/tmp/lj-reclaim-fair-pass-proof-20260905-64ihi1r2')
dest = repo/'notes/evidence/gc-construction-defer-fairness-2026-09-05'
entries, seen, verified = [], set(), []

def digest(data):
    return hashlib.sha256(data).hexdigest()

def add(label, base, rel, expected=None, storage=None):
    assert '..' not in Path(rel).parts and not Path(rel).is_absolute()
    key = str(Path(label)/rel)
    if key in seen:
        return
    source = base/rel
    data = source.read_bytes()
    sha = digest(data)
    if expected is not None:
        assert sha == expected, source
    binary = b'\0' in data or data.startswith((b'\x7fELF', b'!<arch>\n'))
    if not binary:
        try:
            data.decode('utf-8')
        except UnicodeDecodeError:
            binary = True
    if storage == 'text':
        assert not binary, source
    if storage == 'hash-only':
        binary = True
    row = dict(source=str(source), path=key, sha256=sha, bytes=len(data),
               storage='hash-only' if binary else 'text')
    if not binary:
        target = dest/key
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        assert digest(target.read_bytes()) == sha
    entries.append(row)
    seen.add(key)

specs = [
    ('focused', focused, 'source-manifest.json', 'b82c0e15e79c9b755bb469367d21bcdc3d5110b99c98f88bd7572600a611d07c', 'path'),
    ('focused', focused, 'focused-manifest.json', 'd02f89df5ab80c7a5b3a0b68ad24b73593078c24d82884770567745930e5da89', 'path'),
    ('broad', broad, 'manifest.json', 'f70f3f2c88c405844c0953f158822f9175150df2c5d178c5e51cf4d72d2921ee', 'path'),
    ('source-review', proof, 'artifact-manifest.json', 'fc7f95281ca5278f6ee4de340693a31dbd7029b43f2da9569a274fef7d541849', 'relative_path'),
]
for label, base, name, sha, key in specs:
    data = (base/name).read_bytes()
    assert digest(data) == sha
    m = json.loads(data)
    for item in m['artifacts']:
        f = base/item[key]
        assert f.stat().st_size == item['bytes'], f
        add(label, base, item[key], item['sha256'], item['storage'])
    add(label, base, name, sha, 'text')
    verified.append(dict(package=str(base), manifest=name, sha256=sha,
                         entries=len(m['artifacts'])))

source_checks = []
old = json.loads((broad/'input-identities.json').read_text())['source_inputs']
source_sets = [('focused', focused/'candidate', old), ('broad-asan', broad/'asan', old)]
for name, item in json.loads((broad/'supplement-input-identities.json').read_text()).items():
    source_sets.append((name, broad/name, item['inputs']))
for name, tree, inputs in source_sets:
    for rel, sha in inputs.items():
        assert digest((tree/rel).read_bytes()) == sha, (name,rel)
    source_checks.append(dict(name=name, tree=str(tree), inputs=len(inputs)))

setup = json.loads((p/'setup.json').read_text())
for name in ['candidate', 'strict', 'asan']:
    for rel, sha in setup['combined_inputs'].items():
        assert digest((p/name/rel).read_bytes()) == sha, (name, rel)
    source_checks.append(dict(name='root-'+name, tree=str(p/name),
                              inputs=len(setup['combined_inputs'])))
for rel, sha in setup['combined_inputs'].items():
    assert digest((repo/rel).read_bytes()) == sha, rel
initial = [json.loads((p/('results-'+name)/'summary.json').read_text())
           for name in ['candidate','strict','asan']]
extra = [json.loads((p/('broad-'+name)/'summary.json').read_text())
         for name in ['strict','asan']]
assert sum(x['runtime_pass'] for x in initial)==39
assert sum(x['runtime_pass'] for x in extra)==82
assert all(not x['failures'] for x in initial+extra)
canonical = json.loads((p/'canonical.json').read_text())
assert len(canonical)==3 and all(x['exit']==0 for x in canonical)
assert sum(x['expected_runtime_processes'] for x in canonical)==49
cost = json.loads((p/'perf/cost-results.json').read_text())
assert len(cost)==42 and all(x['exit']==0 for x in cost)
final = dict(verified_manifests=verified, source_checks=source_checks,
             root_runtime_passes=170, initial_passes=39, broader_passes=82,
             canonical_passes=49, cost_processes=42, cost_samples=210,
             runtime_base=setup['runtime_base'], source_head=setup['head'],
             source_delta=setup['source_delta'])
(p/'final-validation.json').write_text(json.dumps(final,indent=2)+'\n')
exclude = {'base','candidate','strict','asan'}
for item in sorted(p.iterdir()):
    if item.name in exclude:
        continue
    files = [item] if item.is_file() else sorted(f for f in item.rglob('*') if f.is_file())
    for f in files:
        add('root',p,str(f.relative_to(p)))
for rel in list(setup['source_delta']) + [
    'tests/t-gc2-constructor-defer.c','tests/t-gc2-constructor-mixed.c',
    'tests/t-gc2-constructor-fairness.c','tests/suites/m3_gc.lua']:
    add('integrated',repo,rel)
totals = dict(text=sum(e['storage']=='text' for e in entries),
              hash_only=sum(e['storage']=='hash-only' for e in entries),
              text_bytes=sum(e['bytes'] for e in entries if e['storage']=='text'))
manifest = dict(runtime_base=setup['runtime_base'], source_head=setup['head'],
                totals=totals, verified_manifests=verified, entries=entries)
dest.mkdir(parents=True,exist_ok=True)
(dest/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(totals))
