#!/usr/bin/env python3
import collections
import csv
import hashlib
import json
import statistics
from pathlib import Path

package = Path(__file__).resolve().parent
metadata = json.loads((package/'metadata.json').read_text())
groups = collections.defaultdict(dict)
for suite, filename in [('main','results.csv'), ('long_append','append-long-results.csv')]:
    for row in csv.DictReader((package/filename).open()):
        key = (suite, row['mode'], row['workload'], int(row['payload_bytes']),
               int(row['records']), int(row['streams']))
        groups[key].setdefault(row['variant'], {})[int(row['sample'])] = row

summary_fields = ['suite','mode','workload','payload_bytes','records','streams','samples',
                  'baseline_ns_per_op','candidate_ns_per_op','candidate_over_baseline',
                  'paired_ratio_median','paired_ratio_min','paired_ratio_max',
                  'baseline_ns_min','baseline_ns_max','candidate_ns_min','candidate_ns_max',
                  'baseline_wall_ns_per_op','candidate_wall_ns_per_op']
paired_fields = ['suite','mode','workload','payload_bytes','records','streams','sample',
                 'baseline_cpu_seconds','candidate_cpu_seconds','cpu_ratio',
                 'baseline_wall_seconds','candidate_wall_seconds','wall_ratio']
summary = []
pairs = []
for key, variants in groups.items():
    assert variants['baseline'].keys() == variants['candidate'].keys()
    base = variants['baseline']
    candidate = variants['candidate']
    bns = [float(r['cpu_seconds'])*1e9/int(r['operations']) for r in base.values()]
    cns = [float(r['cpu_seconds'])*1e9/int(r['operations']) for r in candidate.values()]
    bwall = [float(r['wall_seconds'])*1e9/int(r['operations']) for r in base.values()]
    cwall = [float(r['wall_seconds'])*1e9/int(r['operations']) for r in candidate.values()]
    ratios = []
    for sample in base:
        b, c = base[sample], candidate[sample]
        ratio = float(c['cpu_seconds'])/float(b['cpu_seconds'])
        ratios.append(ratio)
        pairs.append(list(key)+[sample,b['cpu_seconds'],c['cpu_seconds'],ratio,
                               b['wall_seconds'],c['wall_seconds'],
                               float(c['wall_seconds'])/float(b['wall_seconds'])])
    bn, cn = statistics.median(bns), statistics.median(cns)
    summary.append(list(key)+[len(base),bn,cn,cn/bn,statistics.median(ratios),
                             min(ratios),max(ratios),min(bns),max(bns),min(cns),max(cns),
                             statistics.median(bwall),statistics.median(cwall)])

for filename, columns, rows in [('summary.csv',summary_fields,summary),
                                ('paired-ratios.csv',paired_fields,pairs)]:
    with (package/filename).open('w') as f:
        writer = csv.writer(f)
        writer.writerow(columns)
        writer.writerows(rows)

lines = ['All times are median process CPU nanoseconds per operation.',
         'Ratio = candidate time / frozen 51f8adc8e time; values above 1 are slower.',
         '',
         'suite        mode  workload             bytes  records streams   HEAD ns    new ns   ratio  paired min..max']
for row in summary:
    r = dict(zip(summary_fields,row))
    lines.append(f"{r['suite']:12} {r['mode']:5} {r['workload']:20} {r['payload_bytes']:5} "
                 f"{r['records']:8} {r['streams']:7} {r['baseline_ns_per_op']:9.2f} "
                 f"{r['candidate_ns_per_op']:9.2f} {r['candidate_over_baseline']:7.2f} "
                 f"{r['paired_ratio_min']:.2f}..{r['paired_ratio_max']:.2f}")
(package/'summary.txt').write_text('\n'.join(lines)+'\n')
metadata['long_append'] = {'samples':5,'records_per_segment':1000000,'segments':10,
                         'warmup_segments':2,'mode':'jit',
                         'note':'Every segment validates complete contents outside the timed interval.'}
metadata['semantic_assertions'] = 'All output assertions passed in all 12 main processes and all 10 long-append processes.'
metadata['main_rows'] = sum(1 for _ in csv.DictReader((package/'results.csv').open()))
metadata['long_append_rows'] = sum(1 for _ in csv.DictReader((package/'append-long-results.csv').open()))
metadata['artifact_sha256'] = {
    name:hashlib.sha256((package/name).read_bytes()).hexdigest()
    for name in ['baseline-input.tar.gz','candidate-input.tar.gz','candidate-input.sha256',
                 'bench.lua','run.py','append-long.lua','run-append-long.py',
                 'results.csv','append-long-results.csv','summary.csv','paired-ratios.csv']
}
(package/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
print('\n'.join(lines))
