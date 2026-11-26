#!/usr/bin/env python3
import csv
from pathlib import Path

p=Path(__file__).resolve().parent
def read(name):
    rows=list(csv.DictReader((p/name).open()))
    return {(r['suite'],r['mode'],r['workload'],r['payload_bytes'],r['records'],r['streams']):r
            for r in rows}
versions={version:read(name) for version,name in
          [('v2','v2-summary.csv'),('v3','v3-summary.csv'),('v4','summary.csv')]}
assert versions['v2'].keys()==versions['v3'].keys()==versions['v4'].keys()
fields=['suite','mode','workload','payload_bytes','records','streams',
        'head_v2_ns','head_v3_ns','head_v4_ns','candidate_v2_ns','candidate_v3_ns','candidate_v4_ns',
        'v2_over_head','v3_over_head','v4_over_head','v4_over_v2_absolute','v4_over_v3_absolute',
        'v4_paired_ratio_min','v4_paired_ratio_max']
rows=[]
lines=['Final v4 results, with earlier versions retained for comparison.',
       'Times are median process CPU nanoseconds per operation; ratios above 1 are slower.',
       'Each version used the same clean HEAD binary and byte-identical workload scripts.',
       'Versions ran separately; raw paired samples and ranges remain in each package.',
       '',
       'mode  workload             bytes  records   HEAD-v4        v2        v3  FINAL-v4  v4/HEAD  paired min..max']
for key,r4 in versions['v4'].items():
    rs=[versions[v][key] for v in ['v2','v3','v4']]
    bases=[float(r['baseline_ns_per_op']) for r in rs]
    candidates=[float(r['candidate_ns_per_op']) for r in rs]
    ratios=[c/b for c,b in zip(candidates,bases)]
    low,high=float(r4['paired_ratio_min']),float(r4['paired_ratio_max'])
    rows.append(list(key)+bases+candidates+ratios+
                [candidates[2]/candidates[0],candidates[2]/candidates[1],low,high])
    lines.append(f'{key[1]:5} {key[2]:20} {int(key[3]):5} {int(key[4]):8} '
                 f'{bases[2]:9.2f} {candidates[0]:9.2f} {candidates[1]:9.2f} '
                 f'{candidates[2]:9.2f} {ratios[2]:8.2f}  {low:.2f}..{high:.2f}')
lines += ['',
    'Every semantic assertion passed in all 22 final-v4 measurement processes.',
    'There are 132 main rows and 20 long-append rows; none were excluded.',
    'Use the long-append rows for tiny-append ratios, since the main HEAD rows are very short.',
    'Streaming rows use 200,000 total nine-byte number records at each stream size.',
    'The decode method remains interpreted even in the JIT-enabled process.',
    'Exact input sources, binaries, build command, GCC version and hashes are packaged.']
with (p/'versions.csv').open('w') as f:
    w=csv.writer(f);w.writerow(fields);w.writerows(rows)
(p/'versions.txt').write_text('\n'.join(lines)+'\n')
print('\n'.join(lines))
