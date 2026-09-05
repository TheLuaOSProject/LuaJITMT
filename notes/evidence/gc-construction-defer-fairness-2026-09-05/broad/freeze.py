from pathlib import Path
import json,hashlib,subprocess
P=Path(__file__).resolve().parent;F=Path('/tmp/lj-reclaim-fair-pass-20260905-kw8kfdam')
def sha(q):return hashlib.sha256(q.read_bytes()).hexdigest()
focused=json.loads((F/'focused-manifest.json').read_text())
for row in focused['artifacts']:assert sha(F/row['path'])==row['sha256'],row['path']
ident=json.loads((P/'input-identities.json').read_text());supp=json.loads((P/'supplement-input-identities.json').read_text());verified={}
for variant,wants in [('asan',ident['source_inputs']),*[(v,r['inputs']) for v,r in supp.items()]]:
 for rel,want in wants.items():assert sha(P/variant/rel)==want,(variant,rel)
 verified[variant]=len(wants)
checks=[]
for variant in ['asan','arenaassert','arenaasan']:
 argv=['git','apply','--reverse','--check',str(F/'candidate.patch')]
 r=subprocess.run(argv,cwd=P/variant,capture_output=True,text=True);assert r.returncode==0
 checks.append(dict(argv=argv,cwd=str(P/variant),exit_code=r.returncode,stdout=r.stdout,stderr=r.stderr))
(P/'source-verification.json').write_text(json.dumps(dict(focused_manifest=sha(F/'focused-manifest.json'),focused_artifacts_reverified=len(focused['artifacts']),source_inputs_reverified=verified,reverse_checks=checks),indent=2)+'\n')
toolchain=[]
for argv in [['cc','--version'],['clang','--version'],['uname','-a']]:
 r=subprocess.run(argv,capture_output=True,text=True,timeout=60);toolchain.append(dict(argv=argv,exit_code=r.returncode,stdout=r.stdout,stderr=r.stderr))
(P/'toolchain.json').write_text(json.dumps(toolchain,indent=2)+'\n')
variants={'asan','controlasan','arenaassert','arenaasan'};selected=[]
for q in P.rglob('*'):
 if not q.is_file() or q.name=='manifest.json':continue
 rel=q.relative_to(P)
 if len(rel.parts)>1 and rel.parts[0] in variants and rel.parts[1] in {'src','tests','dynasm'}:continue
 selected.append(q)
for variant in variants:
 for rel in ['src/libluajit.a','src/luajit','src/host/minilua','src/host/buildvm','src/lj_gc2.o','src/lj_gc.o','src/lj_gc.c','src/lj_gc.h','src/lj_gc2.c','src/lj_obj.h','src/lj_tg.c','src/lj_thr.c','src/lj_arena.c','src/lj_arena.h']:
  selected.append(P/variant/rel)
art=[]
for q in sorted(set(selected)):
 data=q.read_bytes();binary=data.startswith(b'\x7fELF') or data.startswith(b'!<arch>')
 if not binary:data.decode()
 art.append(dict(path=str(q.relative_to(P)),sha256=sha(q),bytes=len(data),storage='hash-only' if binary else 'text'))
(P/'manifest.json').write_text(json.dumps(dict(status='frozen broad validation; original baseline-matched failures remain open, accepted helper generation passes',package=str(P),artifacts=art),indent=2)+'\n')
print(json.dumps(dict(manifest_sha256=sha(P/'manifest.json'),handoff_sha256=sha(P/'HANDOFF.md'),artifacts=len(art),text=sum(r['storage']=='text' for r in art),hash_only=sum(r['storage']=='hash-only' for r in art)),indent=2))
