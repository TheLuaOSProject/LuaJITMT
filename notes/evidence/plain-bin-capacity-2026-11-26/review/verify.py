from pathlib import Path
import hashlib, json, subprocess
p = Path(__file__).resolve().parent
root = Path('/workspaces/lj-lockless')
def sha(f): return hashlib.sha256(Path(f).read_bytes()).hexdigest()
update = json.loads((p/'fixture-update/metadata.json').read_text())
fixture = update['path']
assert sha(p/'fixture-update/original.c') == update['original_sha256']
assert sha(p/'fixture-update/t-arena-remote-pending.c') == update['corrected_sha256']
assert sha(root/fixture) == update['corrected_sha256']
report = dict(base=json.loads((p/'setup.json').read_text())['base'], builds={},
              corrected_fixture=update, superseded_fixture_checks=[])
for mode in ['gcc', 'assert', 'asan', 'tsan', 'noffi', 'nojit', 'nobuf']:
    tree = p/mode
    meta = json.loads((tree/'build.json').read_text())
    assert meta['exit'] == 0, mode
    for rel, expected in meta['source_inputs'].items():
        assert sha(tree/rel) == expected, (mode, 'frozen', rel)
        if rel == fixture:
            assert expected == update['original_sha256'], mode
        else:
            assert sha(root/rel) == expected, (mode, 'workspace', rel)
    for name, expected in meta['outputs'].items():
        assert sha(tree/'src'/name) == expected, (mode, name)
    runs = []
    for f in sorted(tree.glob('check-*/results.json')):
        for row in json.loads(f.read_text()):
            if f.parent.name == 'check-arena' and row['name'] == 'remote-pending':
                assert mode in ['assert', 'asan', 'tsan'], mode
                assert row['exit'] == (-6 if mode == 'asan' else 0), (mode, row)
                report['superseded_fixture_checks'].append(dict(mode=mode,
                    source_sha256=update['original_sha256'], phase=f.parent.name, **row))
                continue
            assert row['exit'] == 0, (mode, f, row)
            runs.append(dict(phase=f.parent.name, name=row['name'],
                             runtime=row.get('test', True), exit=row['exit']))
    assert runs, mode
    runtime = {k:v for k,v in meta['source_inputs'].items()
               if k == 'Makefile' or k.startswith(('src/', 'dynasm/'))}
    report['builds'][mode] = dict(input_count=len(meta['source_inputs']),
        runtime_input_count=len(runtime), source_mismatches=[],
        fixture_update=fixture,
        binary_sha256=meta['outputs'], checks=runs)
    for ident in sorted(tree.glob('check-*/identities.json')):
        for path, expected in json.loads(ident.read_text()).items():
            assert sha(path) == expected, (mode, ident, path)
    if mode in ['assert', 'asan', 'tsan']:
        assert sum(r['runtime'] and r['phase'] == 'check-pending' for r in runs) == 1
canonical = json.loads((p/'canonical/result.json').read_text())
assert canonical['exit'] == 0 and not canonical['source_mismatches']
for name, expected in canonical['outputs'].items():
    assert sha(root/'src'/name) == expected, name
report['canonical'] = canonical
pending = json.loads((p/'canonical-pending/result.json').read_text())
assert pending['exit'] == 0 and not pending['source_mismatches']
assert pending['outputs'] == canonical['outputs']
for path, expected in json.loads((p/'canonical-pending/inputs.json').read_text()).items():
    assert sha(root/path) == expected, path
report['canonical_corrected_fixture'] = pending
report['runtime_checks'] = sum(sum(r['runtime'] for r in b['checks'])
                               for b in report['builds'].values())
assert report['runtime_checks'] == 88, report['runtime_checks']
assert len(report['superseded_fixture_checks']) == 3
report['tracked_changes'] = subprocess.check_output(['git','diff','--stat'], cwd=root, text=True)
(p/'final-verification.json').write_text(json.dumps(report,indent=2)+'\n')
print('Verified', len(report['builds']), 'builds;', report['runtime_checks'],
      'current runtime invocations; frozen inputs match except the separately frozen test correction;',
      'three original-fixture runs preserved; canonical/default hashes match')
