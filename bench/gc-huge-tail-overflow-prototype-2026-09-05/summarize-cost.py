#!/usr/bin/env python3
import collections
import hashlib
import json
import pathlib
import statistics

P = pathlib.Path(__file__).resolve().parent
raw = json.loads((P / 'cost-results.json').read_text())
assert len(raw) == 380 and all(r['exit'] == 0 and r['status'] == 'complete' for r in raw)

def stat(xs):
    return {'min': min(xs), 'median': statistics.median(xs), 'max': max(xs)}

def decoded(row):
    samples = {x['stage']: x for x in row['data'] if x['type'] == 'sample'}
    alloc = next(x for x in row['data'] if x['type'] == 'allocation')
    free = next((x for x in row['data'] if x['type'] == 'free'), None)
    return samples, alloc, free

groups = collections.defaultdict(dict)
for r in raw:
    key = (r['kind'], r['size'], r['touch'])
    assert (r['pair'], r['variant']) not in groups[key]
    groups[key][r['pair'], r['variant']] = r

summary = []
for (kind, size, touch), rows in sorted(groups.items()):
    pairs = sorted({k[0] for k in rows})
    assert len(pairs) == (3 if kind == 'runtime' else 7)
    result = {'kind': kind, 'size': size, 'touch': touch, 'pairs': len(pairs), 'variants': {}}
    measurements = {}
    for variant in ('calloc-normal', 'tail-normal'):
        parsed = [decoded(rows[p, variant]) for p in pairs]
        measures = {'ns_per_alloc': [x[1]['ns_per_alloc'] for x in parsed],
                    'mapsize': [x[1]['mapsize'] for x in parsed],
                    'objects': [x[1]['n'] for x in parsed],
                    'reserved_bytes': [x[1].get('reserved_bytes', x[1].get('reserved_userdata_mapping_bytes')) for x in parsed]}
        if parsed[0][2] is not None:
            measures['ns_per_free'] = [x[2]['ns_per_free'] for x in parsed]
        for stage in parsed[0][0]:
            if stage == 'before':
                continue
            for field in parsed[0][0][stage]:
                if field in ('type', 'stage'):
                    continue
                # Counts/counters are cumulative from the common pre-loop point.
                # Keep both absolute snapshots and within-process changes.
                measures[f'{stage}.{field}'] = [x[0][stage][field] for x in parsed]
                measures[f'{stage}.delta.{field}'] = [x[0][stage][field] - x[0]['before'][field] for x in parsed]
        measurements[variant] = measures
        result['variants'][variant] = {k: stat(v) for k, v in measures.items()}
    before, after = measurements['calloc-normal'], measurements['tail-normal']
    result['paired_difference_tail_minus_calloc'] = {k: stat([b - a for a, b in zip(before[k], after[k])]) for k in before}
    result['paired_alloc_percent'] = stat([100 * (b / a - 1) for a, b in zip(before['ns_per_alloc'], after['ns_per_alloc'])])
    if 'ns_per_free' in before:
        result['paired_free_percent'] = stat([100 * (b / a - 1) for a, b in zip(before['ns_per_free'], after['ns_per_free'])])
    summary.append(result)

out = {
    'raw_sha256': hashlib.sha256((P / 'cost-results.json').read_bytes()).hexdigest(),
    'records': len(raw), 'groups': len(summary), 'summary': summary,
    'method': 'Median/min/max of fresh-process samples and same-pair tail/calloc ratios; CPU 31; seven alternating pairs for direct maps, three for GC-enabled userdata. Within-process retained deltas are paired for RSS/fault comparisons. These are finite local experiments, not confidence intervals or a system isolation claim.'
}
(P / 'cost-summary.json').write_text(json.dumps(out, indent=2) + '\n')
lines = ['| Kind | Logical bytes | Touch | calloc ns | tail ns | Paired change % (min / median / max) | Retained RSS delta KiB (calloc / tail) | Tail minus calloc RSS KiB | Reserved MiB (calloc / tail) |',
         '|---|---:|---|---:|---:|---|---|---:|---|']
for x in summary:
    a, b = x['variants']['calloc-normal'], x['variants']['tail-normal']
    pc = x['paired_alloc_percent']
    lines.append(f"| {x['kind']} | {x['size']} | {x['touch']} | {a['ns_per_alloc']['median']:.0f} | {b['ns_per_alloc']['median']:.0f} | {pc['min']:+.1f} / {pc['median']:+.1f} / {pc['max']:+.1f} | {a['retained.delta.rss_kb']['median']:.0f} / {b['retained.delta.rss_kb']['median']:.0f} | {x['paired_difference_tail_minus_calloc']['retained.delta.rss_kb']['median']:+.0f} | {a['reserved_bytes']['median']/1048576:.0f} / {b['reserved_bytes']['median']/1048576:.0f} |")
(P / 'cost-summary.md').write_text('\n'.join(lines) + '\n')
print('\n'.join(lines))
print('\nCOUNTS AND SETTLED')
for x in summary:
    if x['kind'] not in ('traversable', 'runtime') or x['touch'] == 'untouched':
        continue
    a, b = x['variants']['calloc-normal'], x['variants']['tail-normal']
    end = 'closed' if x['kind'] == 'runtime' else 'freed'
    print(x['kind'], x['size'], x['touch'],
          'W calloc/free', [a[f'retained.calloc_1_16']['median'], a[f'{end}.free_1_16']['median']],
          [b[f'retained.calloc_1_16']['median'], b[f'{end}.free_1_16']['median']],
          'retained minor paired', x['paired_difference_tail_minus_calloc']['retained.delta.minor_faults'],
          'post release RSS paired', x['paired_difference_tail_minus_calloc'][('released_collected' if x['kind'] == 'runtime' else 'freed') + '.delta.rss_kb'])
