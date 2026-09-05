from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import statistics
import subprocess
import time

root = Path(__file__).resolve().parent
repo = Path('/workspaces/lj-lockless')
trees = {'base': Path('/tmp/lj-poll-callback-combined-20260905-xke82c4_/normal'),
         'candidate': root/'normal'}
def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()
paths = subprocess.check_output(['git', 'ls-files', 'src', 'dynasm'], cwd=repo, text=True).splitlines()
differences = [name for name in paths if sha(trees['base']/name) != sha(trees['candidate']/name)]
assert differences == ['src/lj_jit.h', 'src/lj_opt_loop.c', 'src/lj_opt_mem.c'], differences
workload = root/'bench.lua'
shutil.copy2('/tmp/lj-premt-cdata-hoist-20260905-oa96m15y/workloads/bench.lua', workload)
assert sha(workload) == '03fbdcfaa5c5416775a9b87f639322ccb0f97c4cf6ecb83bdf691e7307c20ffa'
rows = []
for pair in range(7):
    for variant in (['base', 'candidate'] if pair % 2 == 0 else ['candidate', 'base']):
        tree = trees[variant]
        command = ['taskset', '-c', '31', str(tree/'src/luajit'), '-jon', str(workload), 'ffi_struct']
        override = {'LUA_PATH': str(tree/'src/?.lua')+';;', 'BENCH_SCALE': '1'}
        start = time.monotonic()
        p = subprocess.run(command, cwd=tree, env=dict(os.environ, **override),
                           capture_output=True, text=True, timeout=30)
        rows.append(dict(pair=pair+1, variant=variant, command=command, cwd=str(tree),
                         environment=override, exit=p.returncode, stdout=p.stdout,
                         stderr=p.stderr, wall_seconds=time.monotonic()-start,
                         runtime_sha256=sha(tree/'src/luajit')))
        (root/'field-cost-results.json').write_text(json.dumps(rows, indent=2)+'\n')
        assert p.returncode == 0, p.stderr
        print(pair+1, variant, p.stdout.splitlines()[-1], flush=True)
times = {v: [float(re.search(r'ffi_struct\s+(\d+\.\d+)', x['stdout']).group(1))
             for x in rows if x['variant']==v] for v in trees}
medians = {k: statistics.median(v) for k, v in times.items()}
summary = dict(fresh_alternating_pairs=7, iterations=30000000, cpu=31,
               statistic='median of seven fresh-process best-of-five printed CPU times',
               times_seconds=times, median_seconds=medians,
               change_percent=100*(medians['candidate']/medians['base']-1),
               source_differences=differences, compared_runtime_inputs=len(paths),
               workload_sha256=sha(workload),
               scope='Normal default mixed runtimes; current poll/callback fixes in both. Only pure-cdata optimizer differs. Shared host; four-decimal workload output. No broader speed or stock-parity claim.')
(root/'field-cost-summary.json').write_text(json.dumps(summary, indent=2)+'\n')
print(json.dumps(summary, indent=2), flush=True)
