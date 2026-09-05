#!/usr/bin/env python3
import hashlib
import json
import pathlib
import subprocess
import tarfile

P = pathlib.Path(__file__).resolve().parent
base = '28de50a622e489019fa22845d6454e029b210582'
manifest = json.loads((P / 'source-manifest.json').read_text())
sha = lambda data: hashlib.sha256(data).hexdigest()
with tarfile.open(P / 'base.tar') as tf:
    originals = {x.name: tf.extractfile(x).read() for x in tf.getmembers() if x.isfile() and x.name.startswith('src/')}
inventory = {'base': base, 'base_tar_sha256': sha((P / 'base.tar').read_bytes()), 'variants': {}, 'patch_checks': []}
for variant, family in [('calloc', 'calloc'), ('calloc-normal', 'calloc'), ('tail', 'tail'), ('tail-normal', 'tail'), ('tail-asan', 'tail')]:
    expected = {x['path']: x['sha256'] for x in manifest['variants'][family]['files']}
    entries, changed = [], []
    for path, original in sorted(originals.items()):
        actual = (P / variant / path).read_bytes()
        ah, bh = sha(actual), sha(original)
        assert ah == expected.get(path, bh), (variant, path, ah, expected.get(path, bh))
        entries.append({'path': path, 'sha256': ah})
        if ah != bh:
            changed.append(path)
    inventory['variants'][variant] = {'files': entries, 'changed_from_base': changed, 'family': family}
    assert sorted(changed) == sorted(expected)

check = P / 'patch-check'
check.mkdir(exist_ok=True)
for path in manifest['variants']['tail']['files']:
    name = path['path']
    dest = check / name
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(originals[name])
for cwd, patch in [(check, 'tail-integrated.patch'), (check, 'calloc-integrated.patch'), (P / 'calloc', 'tail-vs-calloc.patch')]:
    cmd = ['git', 'apply', '--check', str(P / patch)]
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    inventory['patch_checks'].append({'command': cmd, 'cwd': str(cwd), 'exit': r.returncode, 'stdout': r.stdout, 'stderr': r.stderr, 'patch_sha256': sha((P / patch).read_bytes())})
    assert r.returncode == 0, r.stderr
flags = 'a->hdr.flags = LJ_AF_HUGE_MAGIC |\n    (flags & (LJ_AF_FLAG_MASK & ~LJ_AF_EMPTY_RECLAIMED));'
assert flags in (P / 'tail/src/lj_arena.c').read_text()
assert flags in (P / 'calloc/src/lj_arena.c').read_text()
inventory['empty_reclaimed_preserved'] = flags
(P / 'source-inventory.json').write_text(json.dumps(inventory, indent=2) + '\n')
print('Verified', len(originals), 'tracked src files per variant, five positive variants, exactly four changed production files each.')
print('Three patch dry runs passed; exact current EMPTY_RECLAIMED exclusion preserved.')
