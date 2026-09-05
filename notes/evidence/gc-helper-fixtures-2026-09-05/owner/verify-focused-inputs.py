from pathlib import Path
import hashlib, json, subprocess

p = Path(__file__).resolve().parent
repo = Path('/workspaces/lj-lockless')
combined = Path('/tmp/lj-clib-cdata-combined-20260905-bxrxos7h')
old = json.loads((p/'runtime-source-identity.json').read_text())
sha = lambda f: hashlib.sha256(Path(f).read_bytes()).hexdigest()
gitsha = lambda commit, name: hashlib.sha256(subprocess.check_output(
    ['git', 'show', commit+':'+name], cwd=repo)).hexdigest()
commit793 = subprocess.check_output(['git', 'rev-parse', '79345529'], cwd=repo, text=True).strip()
rows = {}
for name, values in old['sources'].items():
    expected843 = values['commit843_sha256']
    release = sha(p/'releasehelpers-v2'/name)
    expected793 = gitsha(commit793, name)
    strict = sha(combined/'strict'/name)
    asan = sha(combined/'asan'/name)
    assert release == expected843, name
    assert expected793 == strict == asan, name
    rows[name] = {'commit843_sha256': expected843, 'releasehelpers_sha256': release,
                  'commit793_sha256': expected793, 'combined_strict_sha256': strict,
                  'combined_asan_sha256': asan}
delta = [name for name, row in rows.items() if row['commit843_sha256'] != row['commit793_sha256']]
assert delta == ['src/lj_crecord.c', 'src/lj_ircall.h', 'src/lj_tab.c', 'src/lj_tab.h'], delta
data = {'original_commit': old['commit'], 'combined_commit': commit793,
        'count': len(rows), 'changed_843_to_793': delta, 'sources': rows,
        'binaries': {}, 'build_records': {}, 'fixture_inputs': {}}
for name, tree in [('843-optimized-helpers', p/'releasehelpers-v2'),
                   ('793-strict', combined/'strict'), ('793-asan', combined/'asan')]:
    data['binaries'][name] = {str(tree/f): sha(tree/f)
                             for f in ['src/luajit', 'src/libluajit.a', 'src/jit/vmdef.lua']}
for f in [p/'releasehelpers-v2-build.json', combined/'strict-build.json', combined/'asan-build.json']:
    data['build_records'][str(f)] = {'sha256': sha(f), 'record': json.loads(f.read_text())}
for name in ['t-gc2-interp-hard-check.c', 't-gc2-alloc-account.c']:
    for d in [p, p/'candidate-v4']:
        data['fixture_inputs'][str(d/name)] = sha(d/name)
data['fixture_inputs'][str(p/'lib/lua_fixture_helpers.h')] = sha(p/'lib/lua_fixture_helpers.h')
out = p/'focused-input-identity.json'
assert not out.exists()
out.write_text(json.dumps(data, indent=2)+'\n')
print('verified', len(rows), 'inputs across 843 optimized and 793 strict/ASan; delta:', delta)
