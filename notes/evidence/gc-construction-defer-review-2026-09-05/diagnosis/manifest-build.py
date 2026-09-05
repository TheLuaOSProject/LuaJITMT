from pathlib import Path
import hashlib,json,shutil
P=Path('/tmp/lj-func-construction-timeout-20260905-8htcwnmd')
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
origin=Path('/tmp/lj-gc-auto-control-20260905-qs673ryl/evidence/artifact-manifest.json')
assert sha(origin)=='5bdc2b9431e12a3d769d579b2ab09da3e68074b72258650b9c1fe81274351300'
copy=P/'prior/string-agent/artifact-manifest.json'
shutil.copyfile(origin,copy)
records=json.loads((P/'independent-observations.json').read_text())
records.append({'original':str(origin),'copy':str(copy.relative_to(P)),'sha256':sha(origin),'bytes':origin.stat().st_size,'note':'Resolved manifest under evidence/; earlier root manifest.json lookup was absent.'})
(P/'independent-observations.json').write_text(json.dumps(records,indent=2)+'\n')
selected=[]
for f in sorted(P.rglob('*')):
 if not f.is_file():continue
 rel=f.relative_to(P)
 if rel.parts[0] in ('debug-four','debug-six') or str(rel)=='manifest.json':continue
 selected.append(f)
for tree in ('debug-four','debug-six'):
 for rel in ('src/libluajit.a','src/luajit','src/lj_gc.o','src/lj_gc2.o','src/lj_func.o'):
  selected.append(P/tree/rel)
art=[]
for f in selected:
 data=f.read_bytes();binary=data.startswith((b'\x7fELF',b'!<arch>\n'))
 if not binary:data.decode('utf-8')
 art.append({'path':str(f.relative_to(P)),'sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data),'storage':'hash-only' if binary else 'text'})
manifest={'package':str(P),'status':'frozen diagnosis and unimplemented source proposal','notes':['No shared source/build/fixture edits.','Full private debug trees remain available; all 807 source inputs per tree verified in final-source-verification.json. Selected reviewed sources included once.','ELF/archive contents hash-only; debugger exit 0 is not a fixture pass.','Every generation, native pass, matched timeout and diagnostic limitation retained.'], 'artifacts':art}
(P/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
for row in art:
 f=P/row['path'];assert sha(f)==row['sha256']
print(json.dumps({'manifest_sha256':sha(P/'manifest.json'),'artifacts':len(art),'text':sum(a['storage']=='text' for a in art),'hash_only':sum(a['storage']=='hash-only' for a in art),'handoff_sha256':sha(P/'HANDOFF.md'),'proposal_sha256':sha(P/'PROPOSAL.md')},indent=2))
