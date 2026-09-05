#!/usr/bin/env python3
"""Verify immutable source inputs and prior evidence; freeze this source audit."""
import datetime
import hashlib
import json
from pathlib import Path
import subprocess

P = Path(__file__).resolve().parent
R = Path('/workspaces/lj-lockless')
setup = json.loads((P / 'setup.json').read_text())


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(name, value):
    (P / name).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


checks = {}
base_bad, shared_differences = [], []
for name, identity in setup['source_inputs'].items():
    current_base = sha(P / 'base' / name)
    shared = sha(R / name)
    checks[name] = {'expected': identity['sha256'], 'base': current_base,
                    'shared_now': shared}
    if current_base != identity['sha256']:
        base_bad.append(name)
    if shared != identity['sha256']:
        shared_differences.append(name)
assert len(checks) == 225 and not base_bad
write('source-verification.json', {
    'checked_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'base_head': setup['head'], 'base_checked': len(checks),
    'base_mismatches': base_bad,
    'shared_head_now': subprocess.check_output(
        ['git', 'rev-parse', 'HEAD'], cwd=R, text=True).strip(),
    'shared_differences_now': shared_differences, 'checks': checks,
})

prior = Path('/tmp/lj-jit-foreground-design-20260905-hwhdaa4a')
manifest_hash = sha(prior / 'artifact-manifest.json')
assert manifest_hash == '8d9bb4a2e6757d479253b58a8a2e879d96af95ccfc730d646730c7b5864b8d3e'
old = json.loads((prior / 'artifact-manifest.json').read_text())
old_bad = [name for name, identity in old['entries'].items()
           if sha(prior / name) != identity['sha256'] or
           (prior / name).stat().st_size != identity['bytes']]
assert not old_bad
write('prior-evidence-verification.json', {
    'package': str(prior), 'manifest_sha256': manifest_hash,
    'checked_entries': len(old['entries']), 'mismatches': old_bad,
    'runtime_source_edits': 0, 'shared_edits': 0,
    'fixture_edits': 0, 'builds': 0, 'runtime_tests': 0,
})

excluded = {'artifact-manifest.json', 'manifest-verification.json'}
entries = {str(path.relative_to(P)):
           {'sha256': sha(path), 'bytes': path.stat().st_size}
           for path in sorted(P.rglob('*'))
           if path.is_file() and path.name not in excluded}
write('artifact-manifest.json', {
    'frozen_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'entries': entries, 'count': len(entries),
    'bytes': sum(identity['bytes'] for identity in entries.values()),
})
frozen_hash = sha(P / 'artifact-manifest.json')
assert all(sha(P / name) == identity['sha256'] and
           (P / name).stat().st_size == identity['bytes']
           for name, identity in entries.items())
write('manifest-verification.json', {
    'manifest_sha256': frozen_hash, 'checked_entries': len(entries),
    'mismatches': [],
})
print('manifest', frozen_hash, 'entries', len(entries))
for name in ('HANDOFF.md', 'RECEIPT-MAP.md', 'REFACTOR.md'):
    print(name, sha(P / name))
print('source_inputs', len(checks), 'prior_entries', len(old['entries']))
print('shared_differences', shared_differences)
