from pathlib import Path
import hashlib,json,time
r=Path(__file__).resolve().parent
base=json.loads((r/'base-identity.json').read_text())
variants=['baseline','baseline-helpers','baseline-asan-helpers','candidate','candidate2','candidate2-assert','candidate2-asan','candidate2-helpers','candidate2-asan-helpers']
result={'base':base['base'],'archive_sha256':hashlib.sha256((r/'runtime-base.tar').read_bytes()).hexdigest(),'variants':{}}
for v in variants:
 changed=[];missing=[];n=0
 expected=dict(base['files'])
 if v=='candidate':expected['src/lj_gc2.c']=hashlib.sha256((r/'source-v1/lj_gc2.c').read_bytes()).hexdigest()
 elif v.startswith('candidate2'):expected['src/lj_gc2.c']=hashlib.sha256((r/'source-v2/lj_gc2.c').read_bytes()).hexdigest()
 for rel,h in expected.items():
  p=r/v/rel
  if not p.is_file():missing.append(rel);continue
  actual=hashlib.sha256(p.read_bytes()).hexdigest();n+=1
  if actual!=h:changed.append({'path':rel,'expected':h,'actual':actual})
 result['variants'][v]={'checked':n,'missing':missing,'unexpected_differences':changed,'intended_gc2_sha256':expected['src/lj_gc2.c']}
 print(v,n,'missing',len(missing),'differences',len(changed),flush=True)
(r/'source-verification.json').write_text(json.dumps(result,indent=2)+'\n')
assert result['archive_sha256']==base['archive_sha256']
assert all(not x['missing'] and not x['unexpected_differences'] for x in result['variants'].values())
