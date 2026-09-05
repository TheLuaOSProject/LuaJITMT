from pathlib import Path
import json, hashlib, shutil
P=Path('/tmp/lj-func-construction-timeout-20260905-8htcwnmd')
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
identity=json.loads((P/'source-identity.json').read_text())
checks={}
for tree in ['debug-six','debug-four']:
 wrong=[]
 for rel,want in identity['inputs'].items():
  q=P/tree/rel
  if not q.exists() or sha(q)!=want: wrong.append(rel)
 checks[tree]={'inputs':len(identity['inputs']),'mismatches':wrong}
 assert not wrong, (tree,wrong)
frozen=json.loads((P/'frozen-inputs.json').read_text())
checked=[]
for variant,items in frozen.items():
 for rel,record in items.items():
  q=Path(record['original']); actual=sha(q)
  assert actual==record['sha256'],(q,actual)
  checked.append({'variant':variant,'path':str(q),'sha256':actual})
checks['original_frozen_inputs']={'count':len(checked),'mismatches':[],'checked':checked}
q=Path('/workspaces/lj-lockless/tests/t-func-construction-anchor.c')
checks['shared_fixture']={'path':str(q),'sha256':sha(q),'matches_frozen':sha(q)==identity['inputs']['tests/t-func-construction-anchor.c']}
assert checks['shared_fixture']['matches_frozen']
(P/'final-source-verification.json').write_text(json.dumps(checks,indent=2)+'\n')
S=Path('/tmp/lj-gc-auto-control-20260905-qs673ryl')
N=P/'prior'/'string-agent';N.mkdir(parents=True,exist_ok=True)
files=['manifest.json','controlhelpers-safety-results.json','controlhelpers-safety-identities.json','function-timeout-gdb-results.json','controlhelpers-function-timeout-gdb.stdout','controlhelpers-function-timeout-gdb.stderr','helpers-function-timeout-gdb.stdout','helpers-function-timeout-gdb.stderr','controlhelpers-build.json','helpers-build.json','source-validation.json','helpers-safety-results.json','helpers-safety-identities.json']
ind=[]
for rel in files:
 q=S/rel
 if not q.exists():
  ind.append({'original':str(q),'missing':True});continue
 data=q.read_bytes()
 assert not data.startswith((b'\x7fELF',b'!<arch>\n'))
 data.decode('utf-8')
 shutil.copyfile(q,N/rel)
 ind.append({'original':str(q),'copy':str((N/rel).relative_to(P)),'sha256':sha(q),'bytes':len(data)})
(P/'independent-observations.json').write_text(json.dumps(ind,indent=2)+'\n')
selected=['src/lj_func.c','src/lj_func.h','src/lj_gc.c','src/lj_gc.h','src/lj_gc2.c','src/lj_gc2.h','src/lj_obj.h','src/lj_arena.c','src/lj_arena.h','src/lj_tg.h','src/lj_safepoint.c','src/Makefile','tests/t-func-construction-anchor.c']
for rel in selected:
 q=P/'reviewed-source'/rel;q.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(P/'debug-six'/rel,q)
(P/'reviewed-source'/'README.txt').write_text('Exact selected source inputs from frozen initial admission candidate, base 597b8705208957ade8465416da30976ab9b52195 plus its three-file prototype. See source-identity.json for all 807 inputs. No runtime source edits. All line references in HANDOFF.md and PROPOSAL.md refer to these files. Four and six helper builds used byte-identical inputs.\n')
print(json.dumps({'verified':{k: {'inputs':v.get('inputs'),'mismatches':v.get('mismatches')} for k,v in checks.items()},'independent_files':ind},indent=2))
