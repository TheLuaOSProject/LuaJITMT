import hashlib, json, pathlib, shutil

r = pathlib.Path('/workspaces/lj-lockless')
p = pathlib.Path(__file__).parent
isolated = pathlib.Path('/tmp/lj-special-udata-pure-20260905-u7z61i10')
combined = pathlib.Path('/tmp/lj-udata-pure-receiver-combined-20260905-sn9vd57b')
dest = r/'notes/evidence/jit-udata-pure-2026-09-05'
sha = lambda q: hashlib.sha256(q.read_bytes()).hexdigest()
entries = []

def add(q, rel, expected=None, storage='text'):
    h = sha(q)
    if expected is not None: assert h == expected, (q,h,expected)
    if storage == 'text':
        assert q.read_bytes()[:4] != b'\x7fELF', q
        assert q.read_bytes()[:8] != b'!<arch>\n', q
        target = dest/rel
        target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(q,target)
        assert sha(target) == h
    entries.append({'source':str(q),'relative_path':str(rel),'sha256':h,
                    'bytes':q.stat().st_size,'storage':storage})

for owner,label in [(isolated,'isolated'),(combined,'combined')]:
    manifest = json.loads((owner/'artifact-manifest.json').read_text())
    for e in manifest['artifacts']:
        assert e['storage'] in ['text','hash-only','sha256-only','hash_only'], e
        q = pathlib.Path(e.get('source',owner/e['relative_path']))
        add(q,pathlib.Path(label)/e['relative_path'],e['sha256'],
            'text' if e['storage']=='text' else 'hash_only')
    add(owner/'artifact-manifest.json',pathlib.Path(label)/'artifact-manifest.json')

sources = json.loads((combined/'runtime-input-identity.json').read_text())
assert sources['count'] == 224
for variant, inputs in sources['combined'].items():
    assert len(inputs)==224, variant
    for name,h in inputs.items(): assert sha(r/name)==h,(variant,name)
canonical = json.loads((p/'canonical.json').read_text())
assert canonical['exit'] == 0
for name,h in canonical['sources'].items(): assert sha(r/name)==h,name
for name,h in canonical['default_binaries'].items(): assert sha(r/'src'/name)==h,name
combined_result=json.loads((combined/'final-validation.json').read_text())
assert combined_result['runtime_processes']==261 and not combined_result['failures']
final = {'setup':json.loads((p/'setup.json').read_text()),'source_count':224,
         'all_shared_runtime_inputs_match_combined':True,
         'combined':combined_result,'canonical_runtime_processes':80,'canonical':canonical}
(p/'final-validation.json').write_text(json.dumps(final,indent=2)+'\n')
for q in sorted(p.iterdir()):
    if q.is_file(): add(q,pathlib.Path('root')/q.name)

manifest = {'artifacts':entries,'text_count':sum(e['storage']=='text' for e in entries),
            'hash_only_count':sum(e['storage']=='hash_only' for e in entries)}
(dest/'artifact-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
bench = r/'bench/jit-udata-pure-2026-09-05'
bench.mkdir(parents=True,exist_ok=True)
bench_entries=[]
for name in ['benchmark.py','cost.lua','direct-cost.lua','cost-results.json','cost-summary.json']:
    q=isolated/name
    shutil.copyfile(q,bench/name)
    bench_entries.append({'source':str(q),'relative_path':name,'sha256':sha(q),
                          'bytes':q.stat().st_size,'storage':'text'})
(bench/'artifact-manifest.json').write_text(json.dumps({'artifacts':bench_entries},indent=2)+'\n')
print(json.dumps({k:v for k,v in manifest.items() if k!='artifacts'}))
