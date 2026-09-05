from pathlib import Path
import json,hashlib,subprocess
P=Path('/tmp/lj-reclaim-fair-pass-20260905-kw8kfdam')
def sha(q):return hashlib.sha256(q.read_bytes()).hexdigest()
prior=json.loads((P/'source-manifest.json').read_text())
for row in prior['artifacts']:assert sha(P/row['path'])==row['sha256'],row['path']
ident=json.loads((P/'source-identity.json').read_text());actual={};changed=[]
for rel,want in ident['inputs'].items():
 actual[rel]=sha(P/'candidate'/rel)
 if actual[rel]!=want:changed.append(rel)
assert changed==['src/lj_gc2.c','src/lj_obj.h'],changed
checks=[]
for patch in ['candidate.patch','fair-delta.patch']:
 argv=['git','apply','--reverse','--check',str(P/patch)]
 r=subprocess.run(argv,cwd=P/'candidate',capture_output=True,text=True)
 checks.append(dict(argv=argv,cwd=str(P/'candidate'),exit_code=r.returncode,stdout=r.stdout,stderr=r.stderr))
 assert r.returncode==0
(P/'focused-source-integrity.json').write_text(json.dumps(dict(source_manifest_reverified=sha(P/'source-manifest.json'),input_count=len(actual),changes_from_rejected=changed,actual_inputs=actual,reverse_checks=checks),indent=2)+'\n')
selected=[q for q in P.rglob('*') if q.is_file() and q.relative_to(P).parts[0]!='candidate' and q.name!='focused-manifest.json']
selected += [P/'candidate'/rel for rel in ['src/lj_gc.c','src/lj_gc.h','src/lj_gc2.c','src/lj_obj.h','src/lj_tg.c','src/lj_thr.h','tests/t-func-construction-anchor.c','src/libluajit.a','src/luajit']]
art=[]
for q in sorted(selected):
 data=q.read_bytes();binary=data.startswith(b'\x7fELF') or data.startswith(b'!<arch>')
 if not binary:data.decode()
 art.append(dict(path=str(q.relative_to(P)),sha256=sha(q),bytes=len(data),storage='hash-only' if binary else 'text'))
(P/'focused-manifest.json').write_text(json.dumps(dict(status='frozen focused acceptance; broad validation pending',package=str(P),artifacts=art),indent=2)+'\n')
print(json.dumps(dict(manifest_sha256=sha(P/'focused-manifest.json'),handoff_sha256=sha(P/'FOCUSED-HANDOFF.md'),artifacts=len(art),text=sum(x['storage']=='text' for x in art),hash_only=sum(x['storage']=='hash-only' for x in art)),indent=2))
