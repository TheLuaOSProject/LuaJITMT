#!/usr/bin/env python3
import datetime
import hashlib
import json
from pathlib import Path

P = Path(__file__).resolve().parent
def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for data in iter(lambda: f.read(1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()
def read(name):
    return json.loads((P / name).read_text())

resolved, failures = {}, []
for variant in ['tail', 'tail-asan']:
    final = {}
    names = [variant + '-validation.json', variant + '-validation-tnew.json']
    if variant == 'tail':
        names.append('tail-validation-terminal-orphan.json')
    for name in names:
        for row in read(name):
            final[row['name']] = {'artifact': name, 'exit': row['exit'], 'status': row['status']}
            if row['exit']:
                failures.append({'artifact': name, 'name': row['name'], 'exit': row['exit'], 'stderr': row['stderr']})
    assert all(r['exit'] == 0 and r['status'] == 'complete' for r in final.values())
    resolved[variant] = final

for name in ['tail-targeted.json', 'tail-asan-targeted.json', 'stock-results.json', 'compile-cost.json']:
    assert all(r['exit'] == 0 for r in read(name))
cost = read('cost-results.json')
assert len(cost) == 380 and all(r['exit'] == 0 and r['status'] == 'complete' for r in cost)
neg = read('negative-results.json')
expected = {'test-old-size': -6, 'test-old-realloc': -6, 'test-body-proof': -11}
assert all(r['exit'] == expected.get(r['name'], 0) for r in neg)

helpers = ['-DLJ_GC2_TEST_HELPERS', '-DLJ_TAB_TEST_HELPERS', '-DLJ_ARENA_TEST_HELPERS', '-DLJ_FUNC_TEST_HELPERS', '-DLJ_TRACE_TEST_HELPERS', '-DLUA_USE_ASSERT']
builds = {}
for variant in ['tail', 'calloc-normal', 'tail-normal', 'tail-asan']:
    command = ['taskset', '-c', '0-15', 'make', '-C', str(P / variant / 'src'), '-j4', 'BUILDMODE=static', 'CCDEBUG=-g', 'TARGET_STRIP=:']
    environment = {}
    if variant in ['tail', 'tail-asan']:
        command.append('XCFLAGS=' + ' '.join(helpers))
    if variant == 'tail-asan':
        command += ['CC=clang', 'CCOPT=-O1 -fno-omit-frame-pointer -fsanitize=address', 'CCOPT_x86=', 'CCOPT_x64=', 'TARGET_LDFLAGS=-fsanitize=address', 'HOST_LDFLAGS=-fsanitize=address']
        environment = {'ASAN_OPTIONS': 'detect_leaks=0'}
    log = 'build-tail-strict.log' if variant == 'tail' else 'build-' + variant + '.log'
    assert 'Successfully built LuaJIT' in (P / log).read_text()
    builds[variant] = {'recorded_session_recipe': command, 'build_generator_environment': environment, 'log': log, 'log_sha256': sha(P / log), 'binaries': {n: sha(P / variant / 'src' / n) for n in ['luajit', 'libluajit.a']}}

manifest = {
    'frozen_at_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'base': read('source-manifest.json')['base'],
    'source_manifest': read('source-manifest.json'),
    'source_inventory_sha256': sha(P / 'source-inventory.json'),
    'builds': builds,
    'asan_runtime_environment': {'ASAN_OPTIONS': 'detect_leaks=1:abort_on_error=1'},
    'runtime_suppressions': [],
    'resolved_authority_arena_cases': resolved,
    'preserved_initial_authority_compile_failures': failures,
    'preserved_other_initial_failures': ['calloc-apply.log', 'targeted-1.log', 'tail-targeted.json.attempt1', 't-huge-tail.c.attempt1', 'compile-cost-first-failure.json'],
    'targeted_final_results': ['tail-targeted.json', 'tail-asan-targeted.json'],
    'negative_results': {'path': 'negative-results.json', 'expected_test_exits': expected},
    'stock': {'path': 'stock-results.json', 'variants': ['calloc-normal', 'tail-normal'], 'each_variant': {'joff': 387, 'jon': 509}},
    'cost': {'raw': 'cost-results.json', 'summary': 'cost-summary.json', 'processes': 380, 'failures_or_timeouts': 0, 'cpu': 31, 'full_system_isolation': False, 'direct_pairs': 7, 'runtime_pairs': 3, 'runtime_gc_suppressed': False, 'runtime_gc_cycles_inside_timed_loop': 0},
    'integration_boundary': 'Frozen 28de plus dense candidate; later tag guard and truthful full FNEW repair are not overlaid. Only isolated high-cell FNEW reuse was executed here. A separate latest-base durable-test patch is required before production integration.',
    'source_or_runtime_changes_after_measurements': False,
    'artifacts': []
}
for path in sorted(P.iterdir()):
    if path.is_file() and path.name not in ['final-validation.json', 'final-validation.sha256']:
        manifest['artifacts'].append({'path': path.name, 'bytes': path.stat().st_size, 'sha256': sha(path)})
(P / 'final-validation.json').write_text(json.dumps(manifest, indent=2) + '\n')
(P / 'final-validation.sha256').write_text(sha(P / 'final-validation.json') + '  final-validation.json\n')
print('Frozen', len(manifest['artifacts']), 'top-level artifacts; source inventory covers five variants.')
print((P / 'final-validation.sha256').read_text(), end='')
