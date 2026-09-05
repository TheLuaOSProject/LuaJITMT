from pathlib import Path
import json,hashlib
P=Path('/tmp/lj-reclaim-owner-defer-20260905-gwiiudxk')
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
old=json.loads((P/'focused-manifest.json').read_text())
for item in old['artifacts']:
 assert sha(P/item['path'])==item['sha256'],item['path']
ident=json.loads((P/'source-identity.json').read_text());S=Path(ident['source_tree']);changed=[]
for rel,want in ident['inputs'].items():
 assert sha(S/rel)==want,rel
 if sha(P/'candidate'/rel)!=want:changed.append(rel)
assert changed==['src/lj_gc.c','src/lj_gc.h','src/lj_gc2.c']
(P/'acceptance-source-validation.json').write_text(json.dumps({'inputs':len(ident['inputs']),'changed':changed,'focused_artifacts_reverified':len(old['artifacts']),'candidate_patch_sha256':sha(P/'candidate.patch')},indent=2)+'\n')
art=[]
for q in sorted(P.rglob('*')):
 if not q.is_file():continue
 rel=q.relative_to(P)
 if str(rel)=='acceptance-manifest.json':continue
 if rel.parts[0]=='candidate' and str(rel) not in ('candidate/src/lj_gc.c','candidate/src/lj_gc.h','candidate/src/lj_gc2.c','candidate/tests/t-func-construction-anchor.c','candidate/src/libluajit.a','candidate/src/luajit'):continue
 data=q.read_bytes();binary=data.startswith((b'\x7fELF',b'!<arch>\n'))
 if not binary:data.decode()
 art.append({'path':str(rel),'bytes':len(data),'sha256':sha(q),'storage':'hash-only' if binary else 'text'})
(P/'acceptance-manifest.json').write_text(json.dumps({'status':'rejected candidate frozen: mixed-owner starvation; positive terminal proofs and all diagnostic generations retained','package':str(P),'artifacts':art},indent=2)+'\n')
print(json.dumps({'manifest_sha256':sha(P/'acceptance-manifest.json'),'artifacts':len(art),'text':sum(x['storage']=='text' for x in art),'hash_only':sum(x['storage']=='hash-only' for x in art),'handoff_sha256':sha(P/'ACCEPTANCE-HANDOFF.md')},indent=2))
