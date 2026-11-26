#!/usr/bin/env python3
import csv
from pathlib import Path

p=Path(__file__).resolve().parent
def read(name):
    rows=list(csv.DictReader((p/name).open()))
    return {(r['suite'],r['mode'],r['workload'],r['payload_bytes'],r['records'],r['streams']):r
            for r in rows}
v2=read('v2-summary.csv')
v3=read('summary.csv')
assert v2.keys()==v3.keys()
fields=['suite','mode','workload','payload_bytes','records','streams',
        'head_v2_ns','head_v3_ns','candidate_v2_ns','candidate_v3_ns',
        'v3_over_v2_absolute','v2_over_head','v3_over_head']
rows=[]
lines=['Every row reports median process CPU nanoseconds per operation.',
       'The same clean HEAD binary and byte-identical workload scripts were used.',
       'Separate-run absolute comparisons are subject to observed background variation;',
       'see paired-ratios.csv and summary.csv for every sample and range.',
       '',
       'mode  workload             bytes  records   HEAD-v2   HEAD-v3        v2        v3   v3/v2  v3/HEAD']
for key,r3 in v3.items():
    r2=v2[key]
    b2=float(r2['baseline_ns_per_op']);b3=float(r3['baseline_ns_per_op'])
    c2=float(r2['candidate_ns_per_op']);c3=float(r3['candidate_ns_per_op'])
    rows.append(list(key)+[b2,b3,c2,c3,c3/c2,c2/b2,c3/b3])
    lines.append(f'{key[1]:5} {key[2]:20} {int(key[3]):5} {int(key[4]):8} '
                 f'{b2:9.2f} {b3:9.2f} {c2:9.2f} {c3:9.2f} {c3/c2:7.3f} {c3/b3:8.2f}')
lines += ['',
    'Observed findings:',
    '- JIT reset/put/get regressed further in v3: 64 B 455.51 -> 528.65 ns;',
    '  1 KiB 1731.75 -> 2017.10 ns. The paired clean HEAD rows were stable.',
    '- The long append rows show no clear absolute speedup: 1 B 60.32 -> 61.27 ns;',
    '  16 B 63.93 -> 67.44 ns. The clean HEAD long rows shifted upward too,',
    '  so separate-run differences should not be treated as isolated causal proof.',
    '- Streaming decode remains linear: v3 JIT-enabled process costs 936.32,',
    '  944.73 and 944.93 ns per record for streams of 1k, 10k and 100k records.',
    '  The method itself remains interpreted. Corresponding HEAD cost is ~819 ns.',
    '- Interpreter medians retain smaller append/read costs, but reset/put/get',
    '  at 1 KiB remains 3.06x HEAD. All samples, including timing outliers, remain',
    '  in the CSV files; no result rows were excluded.',
    '- Every semantic assertion passed in all 22 v3 measurement processes.']
with (p/'comparison-v2-v3.csv').open('w') as f:
    w=csv.writer(f);w.writerow(fields);w.writerows(rows)
(p/'comparison-v2-v3.txt').write_text('\n'.join(lines)+'\n')
print('\n'.join(lines))
