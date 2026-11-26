from pathlib import Path
import hashlib, json, subprocess
p = Path(__file__).resolve().parent
root = Path('/workspaces/lj-lockless')
def sha(f): return hashlib.sha256(Path(f).read_bytes()).hexdigest()
report = dict(base=json.loads((p/'setup.json').read_text())['base'], builds={})
for mode in ['gcc', 'assert', 'asan', 'tsan', 'noffi', 'nojit', 'nobuf']:
    tree = p/mode
    meta = json.loads((tree/'build.json').read_text())
    assert meta['exit'] == 0, mode
    for rel, expected in meta['source_inputs'].items():
        assert sha(tree/rel) == expected, (mode, 'frozen', rel)
        assert sha(root/rel) == expected, (mode, 'workspace', rel)
    for name, expected in meta['outputs'].items():
        assert sha(tree/'src'/name) == expected, (mode, name)
    runs = []
    for f in sorted(tree.glob('check-*/results.json')):
        for row in json.loads(f.read_text()):
            assert row['exit'] == 0, (mode, f, row)
            runs.append(dict(phase=f.parent.name, name=row['name'],
                             runtime=row.get('test', True), exit=row['exit']))
    assert runs, mode
    runtime = {k:v for k,v in meta['source_inputs'].items()
               if k == 'Makefile' or k.startswith(('src/', 'dynasm/'))}
    report['builds'][mode] = dict(input_count=len(meta['source_inputs']),
        runtime_input_count=len(runtime), source_mismatches=[],
        binary_sha256=meta['outputs'], checks=runs)
canonical = json.loads((p/'canonical/result.json').read_text())
assert canonical['exit'] == 0 and not canonical['source_mismatches']
for name, expected in canonical['outputs'].items():
    assert sha(root/'src'/name) == expected, name
report['canonical'] = canonical
report['runtime_checks'] = sum(sum(r['runtime'] for r in b['checks'])
                               for b in report['builds'].values())
report['initial_assertion_trial'] = dict(path='/tmp/lj-plain-shrink-20261126-7izllbqs', failure='New inline lj_assertX lacked the standalone lj_assert_fail declaration; moved validation responsibility to already validated C callers.')
report['tracked_changes'] = subprocess.check_output(['git','diff','--stat'], cwd=root, text=True)
(p/'final-verification.json').write_text(json.dumps(report,indent=2)+'\n')
print('Verified', len(report['builds']), 'builds;', report['runtime_checks'],
      'runtime invocations; all frozen inputs match workspace and canonical/default hashes match')
