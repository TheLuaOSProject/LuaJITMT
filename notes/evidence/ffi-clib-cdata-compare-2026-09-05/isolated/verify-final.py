from pathlib import Path
import hashlib,json
p=Path(__file__).resolve().parent
sha=lambda f:hashlib.sha256(Path(f).read_bytes()).hexdigest()
def manifest(f):
 data=json.loads(f.read_text());root=Path(data['root'])
 for row in data['artifacts']:
  file=root/row['relative_path']
  assert sha(file)==row['sha256'],str(file)
  assert file.stat().st_size==row['bytes'],str(file)
 return {'path':str(f),'sha256':sha(f),'artifacts':len(data['artifacts'])}
results={'prior_manifests':[],'current_manifests':[]}
for dirname in ['lj-clib-hit-cost-review-20260905-cmxsefzb',
                'lj-clib-cdata-compare-proof-20260905-ei_30vvn',
                'lj-clib-cache-regressions-20260905-741ke1nb',
                'lj-special-udata-pure-20260905-u7z61i10',
                'lj-udata-pure-receiver-combined-20260905-sn9vd57b']:
 results['prior_manifests'].append(manifest(Path('/tmp')/dirname/'artifact-manifest.json'))
for name in ['ir-review-manifest.json','validation-frozen-manifest.json']:
 results['current_manifests'].append(manifest(p/name))
identity=json.loads((p/'source-identity-final.json').read_text())
for kind in ['base','candidate','strict','asan']:
 for rel,digest in identity[kind]['runtime_generator_inputs'].items():
  assert sha(p/kind/rel)==digest,(kind,rel)
 for rel,digest in identity[kind].get('binaries',{}).items():
  assert sha(p/kind/rel)==digest,(kind,rel)
baseline=Path('/tmp/lj-clib-cache-root-20260905-i59mqoic/v2/candidate')
for rel,digest in identity['base']['runtime_generator_inputs'].items():
 assert sha(baseline/rel)==digest,rel
summary=json.loads((p/'validation-summary.json').read_text());count=0
for variant in ['candidate','strict','asan']:
 for filename,suite in summary[variant]['suites'].items():
  f=p/'validation'/filename
  assert sha(f)==suite['sha256'],filename
  rows=json.loads(f.read_text());assert len(rows)==suite['count']
  assert all(row['exit']==0 for row in rows),filename
  count+=len(rows)
assert count==summary['total']==594
cost=json.loads((p/'perf/cost-results.json').read_text());assert len(cost)==14
for row in cost:
 assert row['exit']==0
 for f,digest in row['inputs'].items():assert sha(f)==digest,f
 if row['variant']=='baseline':assert row['command'][3]==str(baseline/'src/luajit')
final=p/'artifact-manifest.json'
if final.exists():results['current_manifests'].append(manifest(final))
results['runtime_generator_input_sets']=5
results['inputs_each']=224
results['functional_processes']=count
results['cost_processes']=len(cost)
print(json.dumps(results,indent=2))
