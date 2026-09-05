from pathlib import Path
import json,hashlib,subprocess
P=Path('/tmp/lj-reclaim-fair-pass-20260905-kw8kfdam')
def sha(q):return hashlib.sha256(q.read_bytes()).hexdigest()
ident=json.loads((P/'source-identity.json').read_text());S=Path(ident['starting_source']);changed=[]
for rel,want in ident['inputs'].items():
 assert sha(S/rel)==want,rel
 if sha(P/'candidate'/rel)!=want:changed.append(rel)
assert changed==['src/lj_gc2.c','src/lj_obj.h'],changed
checks=[]
for patch in ['candidate.patch','fair-delta.patch']:
 r=subprocess.run(['git','apply','--reverse','--check',str(P/patch)],cwd=P/'candidate',capture_output=True,text=True)
 assert r.returncode==0,r.stderr
 checks.append({'argv':r.args,'cwd':str(P/'candidate'),'exit_code':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
(P/'source-validation.json').write_text(json.dumps({'input_count':len(ident['inputs']),'delta_from_rejected':changed,'reverse_checks':checks},indent=2)+'\n')
selected=[q for q in P.rglob('*') if q.is_file() and q.relative_to(P).parts[0]!='candidate' and q.name!='source-manifest.json']
selected += [P/'candidate'/rel for rel in ['src/lj_gc.c','src/lj_gc.h','src/lj_gc2.c','src/lj_obj.h','src/lj_tg.c','src/lj_thr.h','tests/t-func-construction-anchor.c']]
art=[]
for q in sorted(selected):
 data=q.read_bytes();data.decode()
 art.append({'path':str(q.relative_to(P)),'sha256':sha(q),'bytes':len(data),'storage':'text'})
(P/'source-manifest.json').write_text(json.dumps({'status':'frozen implementation before validation','package':str(P),'artifacts':art},indent=2)+'\n')
print(json.dumps({'manifest_sha256':sha(P/'source-manifest.json'),'artifacts':len(art),'full_patch':sha(P/'candidate.patch'),'delta_patch':sha(P/'fair-delta.patch')},indent=2))
