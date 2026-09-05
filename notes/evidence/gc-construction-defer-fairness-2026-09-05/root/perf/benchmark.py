from pathlib import Path
import hashlib, json, os, resource, subprocess, sys, time

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
p = Path(__file__).resolve().parent
repo = Path('/workspaces/lj-lockless')
trees = {'baseline': Path('/tmp/lj-clib-cdata-combined-20260905-bxrxos7h/candidate'),
         'candidate': p.parent/'candidate'}
rows = []
pilot = '--pilot' in sys.argv
output = p/('cost-pilot.json' if pilot else 'cost-results.json')
assert not output.exists(), output
workloads = [('tnew', '-joff'), ('tdup', '-joff'), ('tnew', '-jon')]
if pilot:
    workloads = workloads[:3]
for workload, mode in workloads:
    for pair in range(1 if pilot else 7):
        for variant in (['baseline', 'candidate'] if pair % 2 == 0 else
                        ['candidate', 'baseline']):
            tree = trees[variant]
            fixture = p/'allocation-cost.lua' if workload in ('tnew', 'tdup') else repo/'aux/bench/bench.lua'
            args = [str(fixture), workload]
            if workload in ('tnew', 'tdup'):
                args.append('100000')
            envadd = {'LUA_PATH': str(tree/'src/?.lua')+';;', 'BENCH_SCALE': '1'}
            cmd = ['taskset', '-c', '30', str(tree/'src/luajit'), mode] + args
            start = time.monotonic()
            try:
                result = subprocess.run(cmd, cwd=repo, env={**os.environ, **envadd},
                                        capture_output=True, text=True, timeout=45)
                fields = {'exit': result.returncode, 'stdout': result.stdout,
                          'stderr': result.stderr}
            except subprocess.TimeoutExpired as e:
                fields = {'exit': None, 'timeout': True,
                          'stdout': (e.stdout or b'').decode(errors='replace'),
                          'stderr': (e.stderr or b'').decode(errors='replace')}
            rows.append({'workload': workload, 'mode': mode, 'pair': pair,
                         'variant': variant, 'command': cmd, 'cwd': str(repo),
                         'environment': envadd, 'seconds': time.monotonic()-start,
                         'fixture_sha256': hashlib.sha256(fixture.read_bytes()).hexdigest(),
                         'exe_sha256': hashlib.sha256((tree/'src/luajit').read_bytes()).hexdigest(),
                         **fields})
            output.write_text(json.dumps(rows, indent=2)+'\n')
            print(workload, mode, pair, variant, fields['exit'], flush=True)
            assert fields['exit'] == 0, rows[-1]
