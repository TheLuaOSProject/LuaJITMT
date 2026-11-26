#!/usr/bin/env python3
import csv
import subprocess
from pathlib import Path

package = Path(__file__).resolve().parent
bins = {'baseline':package/'baseline-luajit', 'candidate':package/'candidate-luajit'}
with (package/'quiet-append-long-results.csv').open('w') as results:
    results.write((package/'results.csv').read_text().splitlines()[0]+'\n')
    results.flush()
    for sample in range(1,6):
        variants=['baseline','candidate'] if sample%2 else ['candidate','baseline']
        for order,variant in enumerate(variants,1):
            stem=f'quiet-long-{sample}-{order}-{variant}'
            command=['taskset','-c','8',str(bins[variant]),str(package/'append-long.lua'),variant,str(sample),str(order)]
            print('RUN '+stem,flush=True)
            proc=subprocess.run(command,text=True,capture_output=True,timeout=120)
            (package/(stem+'.stdout')).write_text(proc.stdout)
            (package/(stem+'.stderr')).write_text(proc.stderr)
            if proc.returncode:
                raise SystemExit(stem+' failed: '+proc.stderr)
            rows=list(csv.reader(proc.stdout.splitlines()))
            if len(rows)!=2 or any(len(r)!=14 for r in rows):
                raise SystemExit(stem+' malformed CSV')
            results.write(proc.stdout)
            results.flush()
            print('DONE '+stem,flush=True)
print('FINISHED',flush=True)
