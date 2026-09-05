#!/usr/bin/env python3
"""Read-only source/evidence identity audit; freeze this new documentation package."""
import datetime
import hashlib
import json
from pathlib import Path
import subprocess

P = Path(__file__).resolve().parent
setup = json.loads((P / 'setup.json').read_text())
accepted = Path(setup['accepted_source_tree'])
shared = Path('/workspaces/lj-lockless')
prior = Path('/tmp/lj-jit-sweep-diagnosis-20260905-jjdidw9u')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dump(name, value):
    (P / name).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


checks = {}
base_bad, accepted_bad, shared_diff = [], [], []
for name, identity in setup['source_inputs'].items():
    expected = identity['expected']
    base_hash = sha(P / 'base' / name)
    accepted_hash = sha(accepted / name)
    shared_hash = sha(shared / name)
    checks[name] = {'expected': expected, 'base': base_hash,
                    'accepted': accepted_hash, 'shared_now': shared_hash}
    if base_hash != expected:
        base_bad.append(name)
    if accepted_hash != expected:
        accepted_bad.append(name)
    if shared_hash != expected:
        shared_diff.append(name)
assert len(checks) == 225
assert not base_bad and not accepted_bad, (base_bad, accepted_bad)
# Parent integrated the independent scalar-next change after our copy.
assert set(shared_diff).issubset({'src/lj_tab.c', 'src/lj_tab.h'}), shared_diff
dump('source-verification.json', {
    'checked_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'base_checked': len(checks), 'accepted_checked': len(checks),
    'base_mismatches': base_bad, 'accepted_mismatches': accepted_bad,
    'shared_head_now': subprocess.check_output(
        ['git', 'rev-parse', 'HEAD'], cwd=shared, text=True).strip(),
    'shared_differing_paths_now': shared_diff,
    'shared_note': 'Independent scalar-next integration after setup; all other accepted runtime inputs still match.',
    'checks': checks,
})

prior_manifest_path = prior / 'artifact-manifest.json'
prior_manifest_hash = sha(prior_manifest_path)
assert prior_manifest_hash == '407576f5b20f575a802918e517605a9f18522293fc9532d75ff170b0d54d416b'
prior_manifest = json.loads(prior_manifest_path.read_text())
prior_bad = [name for name, identity in prior_manifest['entries'].items()
             if sha(prior / name) != identity['sha256'] or
             (prior / name).stat().st_size != identity['bytes']]
assert not prior_bad, prior_bad
dump('prior-evidence-verification.json', {
    'package': str(prior), 'manifest_sha256': prior_manifest_hash,
    'checked_entries': len(prior_manifest['entries']), 'mismatches': prior_bad,
    'runtime_tests_in_this_checkpoint': 0, 'runtime_edits': 0,
    'fixture_edits': 0, 'builds': 0,
})

excluded = {'artifact-manifest.json', 'manifest-verification.json'}
entries = {str(path.relative_to(P)):
           {'sha256': sha(path), 'bytes': path.stat().st_size}
           for path in sorted(P.rglob('*'))
           if path.is_file() and path.name not in excluded}
dump('artifact-manifest.json', {
    'frozen_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'entries': entries, 'count': len(entries),
    'bytes': sum(identity['bytes'] for identity in entries.values()),
})
manifest_hash = sha(P / 'artifact-manifest.json')
assert all(sha(P / name) == identity['sha256'] and
           (P / name).stat().st_size == identity['bytes']
           for name, identity in entries.items())
dump('manifest-verification.json', {
    'manifest_sha256': manifest_hash, 'checked_entries': len(entries),
    'mismatches': [],
})
print('manifest', manifest_hash, 'entries', len(entries))
for name in ('HANDOFF.md', 'DESIGN.md', 'source-verification.json'):
    print(name, sha(P / name))
print('shared_head', json.loads((P / 'source-verification.json').read_text())['shared_head_now'])
print('shared_differences', shared_diff)
print('prior_evidence_entries', len(prior_manifest['entries']), 'all matched')
