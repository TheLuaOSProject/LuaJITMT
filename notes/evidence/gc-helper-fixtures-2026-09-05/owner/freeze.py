from pathlib import Path
from collections import Counter
import difflib, hashlib, json, subprocess

p = Path(__file__).resolve().parent
sha = lambda f: hashlib.sha256(Path(f).read_bytes()).hexdigest()
def write(name, value):
    f = p/name
    assert not f.exists(), f
    f.write_text(json.dumps(value, indent=2)+'\n')

original = json.loads((p/'inputs.json').read_text())
for name, expected in original['sources'].items():
    assert sha(p/name) == expected, name
delta = ''.join(''.join(difflib.unified_diff(
    (p/name).read_text().splitlines(keepends=True),
    (p/'candidate-v4'/name).read_text().splitlines(keepends=True),
    fromfile='a/tests/'+name, tofile='b/tests/'+name))
    for name in original['sources'])
assert delta == (p/'candidate-v4.patch').read_text()

summary = {'final_candidate': 'candidate-v4', 'results': {}, 'all_compile': {},
           'all_runtime': {}, 'final_runtime': {}, 'original_runtime': {}, 'debuggers': {}}
compiles, runtimes, final, controls = Counter(), Counter(), Counter(), Counter()
for f in sorted(p.glob('*/*results.json')):
    data = json.loads(f.read_text())
    name = str(f.relative_to(p))
    if isinstance(data, dict):
        summary['debuggers'][name] = {
            'driver_exit': data['exit'],
            'target_sigabrt': 'Program received signal SIGABRT' in data['stdout'],
            'status': 'observer failure' if 'optimized out' in data.get('stderr', '') else 'aborted target observation',
            'sha256': sha(f),
        }
        continue
    counts = Counter()
    for r in data:
        status = 'timeout' if r.get('timeout') else 'pass' if r['exit'] == 0 else 'failure'
        counts[r['stage']+'_'+status] += 1
        (compiles if r['stage'] == 'compile' else runtimes)[status] += 1
        if r['stage'] == 'run' and f.parent.name == 'candidate-v4':
            final[status] += 1
        if r['stage'] == 'run' and f.parent.name == 'original':
            controls[status] += 1
    summary['results'][name] = {'counts': dict(counts), 'sha256': sha(f)}
summary.update(all_compile=dict(compiles), all_runtime=dict(runtimes),
               final_runtime=dict(final), original_runtime=dict(controls))
assert final == {'pass': 10}, final
assert controls == {'failure': 6}, controls
assert compiles == {'pass': 33, 'failure': 2}, compiles
assert runtimes == {'pass': 12, 'failure': 21}, runtimes
write('validation-summary.json', summary)

environment = {}
for name, cmd in [('system', ['uname', '-a']), ('cc', ['cc', '--version']),
                  ('clang', ['clang', '--version']), ('make', ['make', '--version']),
                  ('gdb', ['gdb', '--version']), ('python', ['python3', '--version'])]:
    q = subprocess.run(cmd, capture_output=True, text=True)
    environment[name] = {'command': cmd, 'exit': q.returncode, 'stdout': q.stdout, 'stderr': q.stderr}
write('environment.json', environment)
canonical = Path('/tmp/lj-helper-fixtures-root-20260905-wa27ui28/canonical.json')
write('root-canonical-snapshot.json', {'source': str(canonical), 'sha256': sha(canonical),
                                    'record': json.loads(canonical.read_text()),
                                    'note': 'Separate ROOT evidence; excluded from owner counts. m6 is incomplete with a later unchanged idle-reclaim timeout.'})

artifacts = {}
for f in sorted(p.rglob('*')):
    if not f.is_file():
        continue
    rel = f.relative_to(p)
    # Runtime/generator inputs and build outputs have complete independent
    # identity records; retain their trees without duplicating object hashes.
    if rel.parts[0] in ['releasehelpers', 'releasehelpers-v2']:
        continue
    if rel.name in ['artifact-manifest.json', 'final-verification.json']:
        continue
    artifacts[str(rel)] = sha(f)
write('artifact-manifest.json', {'count': len(artifacts), 'artifacts': artifacts,
                               'runtime_identity_records': ['runtime-source-identity.json',
                                  'releasehelpers-v2-inputs.json', 'releasehelpers-v2-build.json',
                                  'focused-input-identity.json']})
print(json.dumps({'manifest_count': len(artifacts), 'manifest_sha256': sha(p/'artifact-manifest.json'),
                  'final_runtime': dict(final), 'original_runtime': dict(controls),
                  'all_runtime': dict(runtimes), 'all_compile': dict(compiles)}, indent=2))
