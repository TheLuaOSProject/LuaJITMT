#!/usr/bin/env python3
import csv
import hashlib
import json
import subprocess
import sys
from pathlib import Path

package = Path(__file__).resolve().parent
metadata = json.loads((package/'metadata.json').read_text())
cpu = metadata['cpu']
bins = {
    'baseline': package/'baseline/src/luajit',
    'candidate': package/'candidate-luajit-v2',
}
metadata['binary_sha256'] = {
    name: hashlib.sha256(path.read_bytes()).hexdigest()
    for name, path in bins.items()
}
metadata['benchmark_sha256'] = hashlib.sha256((package/'bench.lua').read_bytes()).hexdigest()
metadata['samples'] = 3
metadata['warmups_per_row'] = 2
metadata['timing'] = 'os.clock process CPU seconds and CLOCK_MONOTONIC wall seconds'
(package/'metadata.json').write_text(json.dumps(metadata, indent=2)+'\n')
columns = ['variant','mode','workload','sample','order','records','streams',
           'payload_bytes','operations','data_bytes','cpu_seconds','wall_seconds',
           'checksum','trace_count']
with (package/'results.csv').open('w') as results:
    results.write(','.join(columns)+'\n')
    results.flush()
    for sample in range(1, metadata['samples']+1):
        for mode in ['jit', 'joff']:
            variants = ['baseline','candidate'] if sample % 2 else ['candidate','baseline']
            for order, variant in enumerate(variants, 1):
                command = ['taskset','-c',str(cpu),str(bins[variant])]
                if mode == 'joff': command.append('-joff')
                command += [str(package/'bench.lua'),variant,str(sample),str(order)]
                stem = f'{sample}-{mode}-{order}-{variant}'
                print(f'RUN {stem}', flush=True)
                proc = subprocess.run(command, text=True, capture_output=True, timeout=180)
                (package/(stem+'.stdout')).write_text(proc.stdout)
                (package/(stem+'.stderr')).write_text(proc.stderr)
                if proc.returncode:
                    print(proc.stderr, file=sys.stderr)
                    raise SystemExit(f'{stem} exited {proc.returncode}')
                rows = list(csv.reader(proc.stdout.splitlines()))
                if len(rows) != 11 or any(len(r) != len(columns) for r in rows):
                    raise SystemExit(f'{stem} malformed CSV')
                results.write(proc.stdout)
                results.flush()
                print(f'DONE {stem}', flush=True)
print('FINISHED', flush=True)
