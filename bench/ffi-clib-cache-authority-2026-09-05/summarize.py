import json, pathlib, re, statistics, math
p=pathlib.Path(__file__).parent
rows=json.loads((p/'cost-results.json').read_text())
result={}
for work in ['direct_clib','clib','call','ffi_struct','shared']:
    values={v:[] for v in ['baseline','candidate']}
    shapes={v:[] for v in values}
    for row in [x for x in rows if x['workload']==work]:
        assert row['exit']==0
        if work=='ffi_struct':
            line=[line for line in row['stdout'].splitlines() if line.startswith('ffi_struct')][0]
            seconds=float(line.split()[1])
            ns=seconds/30000000*1e9
        else:
            m=re.search(r'^result\s+\w+\s+(\d+)\s+([0-9.]+)(.*)$',row['stdout'],re.M)
            assert m,row
            ns=float(m[2])/int(m[1])*1e9
        values[row['variant']].append(ns)
        shapes[row['variant']].append([line for line in row['stdout'].splitlines()
                                      if line.startswith(('shape','result'))])
    ratios=[b/a for a,b in zip(values['baseline'],values['candidate'])]
    result[work]={'ns':values,'median_ns':{v:statistics.median(a) for v,a in values.items()},
                  'median_paired_percent':statistics.median([(x-1)*100 for x in ratios]),
                  'geometric_ratio':math.exp(statistics.mean([math.log(x) for x in ratios])),
                  'shape_and_result_lines':shapes}
out={'runtime_processes':len(rows),'cpu':31,'pairs_per_workload':7,
     'gc_enabled':True,'results':result,
     'summary_setup_note':'The initial interactive parser assumed extra ffi_struct columns; the stock harness prints total_s and ns/op. This parser uses its measured rounded total_s and unchanged 30 million iterations. Raw timing rows were unchanged.'}
(p/'cost-summary.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps({k:{x:v[x] for x in ['median_ns','median_paired_percent','geometric_ratio']}
                  for k,v in result.items()},indent=2))
